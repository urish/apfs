#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h> /* for malloc/free */
#include <stdio.h> /* for fs_perror */
#include <errno.h> /* for fs_perror */
#include "apfs.h"

#define divup(a,b) (((a) + (b) - 1) / (b))

int fs_errno;

struct {
    int fd;
    block_t blocksize, maxblocks, freeblocks, freeblock;
    block_t *cachedfat;
    int cachedfat_block, cachedfat_modified;
} FSState;

/* Internal functions */
#define check_os_error(expression) \
    if ((expression) < 0) {	\
	fs_errno = FS_EOS;	\
	return;			\
    }

static void read_block (block_t blockid, void *addr) {
    if (blockid >= FSState.maxblocks) {
	fs_errno = FS_ENOBLOCK;
	return;
    }
    check_os_error(lseek(FSState.fd, blockid * FSState.blocksize, SEEK_SET));
    check_os_error(read(FSState.fd, addr, FSState.blocksize));
}

static void write_block (block_t blockid, void *addr) {
    if (blockid >= FSState.maxblocks) {
	fs_errno = FS_ENOBLOCK;
	return;
    }
    check_os_error(lseek(FSState.fd, blockid * FSState.blocksize, SEEK_SET));
    check_os_error(write(FSState.fd, addr, FSState.blocksize));
}

static void zero_block (block_t blockid) {
    char buf[FSState.blocksize];
    memset(buf, 0, sizeof(buf));
    write_block(blockid, buf);
}

static void fat_load_block (block_t block) {
    if (block != FSState.cachedfat_block) {
	if (FSState.cachedfat_modified)
	    write_block(FSState.cachedfat_block, FSState.cachedfat);
	read_block(block, FSState.cachedfat);
	if (fs_errno)
	    return;
	FSState.cachedfat_block = block;
	FSState.cachedfat_modified = 0;
    }
}

static void flush_fat_cache () {
    if (FSState.cachedfat_modified)
        write_block(FSState.cachedfat_block, FSState.cachedfat);
    FSState.cachedfat_modified = 0;
}

static void update_free_space () {
    fat_load_block(0);
    ((FSInfoBlock*)FSState.cachedfat)->freeblocks = FSState.freeblocks;
    FSState.cachedfat_modified = 1;
}

static int read_fatentry (block_t block) {
    int fblock = (block + 8) / (FSState.blocksize / sizeof(short int));
    int foffset = (block + 8) % (FSState.blocksize / sizeof(short int));
    fat_load_block(fblock);
    if (fs_errno)
	return -1;
    return FSState.cachedfat[foffset];
}

static void set_fatentry (block_t block, block_t value) {
    int fblock = (block + 8) / (FSState.blocksize / sizeof(short int));
    int foffset = (block + 8) % (FSState.blocksize / sizeof(short int));
    fat_load_block(fblock);
    if (fs_errno)
	return;
    FSState.cachedfat_modified = 1;
    FSState.cachedfat[foffset] = value;
}

#define check_error()	\
    if (fs_errno)	\
	return;

#define check_error_ret(ret)	\
    if (fs_errno)	\
	return ret;

static block_t block_alloc () {
    block_t newblock = FSState.freeblock;
    if (!newblock) {
	fs_errno = FS_ENOSPACE;
	return 0;
    }
    FSState.freeblock = read_fatentry(newblock);
    check_error_ret(0);
    FSState.freeblocks--;
    set_fatentry(0, FSState.freeblock);
    check_error_ret(0);
    update_free_space();
    check_error_ret(0);
    set_fatentry(newblock, 0);
    return newblock;
}

static block_t allocate_blocks (block_t count) {
    block_t newblock = FSState.freeblock, block = 0;
    int i;
    for (i = count; i; i--) {
	block = block ? read_fatentry(block) : newblock;
        check_error_ret(0);
	if (!block) {
	    fs_errno = FS_ENOSPACE;
	    return 0;
	}
    }
    FSState.freeblock = read_fatentry(block);
    check_error_ret(0);
    FSState.freeblocks -= count;
    set_fatentry(0, FSState.freeblock);
    check_error_ret(0);
    update_free_space();
    check_error_ret(0);
    set_fatentry(block, 0);
    check_error_ret(0);
    return newblock;
}

static void free_blocks (block_t block) {
    block_t oldfree = FSState.freeblock;
    block_t listend, tmp;
    if (!block)
	return;
    for (listend = block; (tmp = read_fatentry(listend)); listend = tmp)
	FSState.freeblocks++;
    FSState.freeblocks++;
    update_free_space();
    check_error();
    set_fatentry(0, block);
    check_error();
    set_fatentry(listend, oldfree);
    check_error();
    FSState.freeblock = block;
}

static block_t read_alloc_fatentry (block_t block, int zero) {
    int nextblock = read_fatentry(block);
    check_error_ret(0);
    if (nextblock)
	return nextblock;
    nextblock = block_alloc();
    check_error_ret(0);
    if (zero) {
	zero_block(nextblock);
	check_error_ret(0);
    }
    set_fatentry(block, nextblock);
    return nextblock;
}

static FSLocation find_dir_space (block_t dir, int length) {
    block_t block = dir;
    FSLocation start;
    int readentries = 0, count = 0;
    FSDirEntry entries[FSState.blocksize / sizeof(FSDirEntry)];
    read_block(block, &entries);
    check_error_ret(((FSLocation){0, 0}));
    while (1) {
	if (entries[readentries].attrs & FATTR_DELETED) {
	    if (!count)
		start = (FSLocation){block, readentries};
	    if (++count == length)
		return start;
	} else if (!entries[readentries].attrs) {
	    return count ? start : (FSLocation){block, readentries};
	} else
	    count = 0;
	if (++readentries == FSState.blocksize / sizeof(FSDirEntry)) {
	    block = read_alloc_fatentry(block, 1);
	    check_error_ret(((FSLocation){0, 0}));
	    read_block(block, &entries);
	    check_error_ret(((FSLocation){0, 0}));
	    readentries = 0;
	}
    }
}

static void dir_free_ent (FSLocation ent) {
    int notfirst = 0, blockmod = 0;
    FSDirEntry entries[FSState.blocksize / sizeof(FSDirEntry)];
    read_block(ent.block, &entries);
    check_error();
    while ((entries[ent.offset].attrs & FATTR_NAMECHUNK) ||
	    (entries[ent.offset].attrs & FATTR_LASTCHUNK) ||
	    !notfirst++) {
	blockmod++;
	entries[ent.offset].attrs |= FATTR_DELETED;
	if (++ent.offset == FSState.blocksize / sizeof(FSDirEntry)) {
	    write_block(ent.block, &entries);
	    check_error();
	    ent.block = read_fatentry(ent.block);
	    if (!ent.block)
		return;
	    read_block(ent.block, &entries);
	    check_error();
	    ent.offset = 0;
	    blockmod = 0;
	}
    }
    if (blockmod)
	write_block(ent.block, &entries);
}

static int process_dir_entry (FSDirEntry *entry, FSFileInfo *result, FSDirSearchInfo *dsinfo, FSLocation dirent) {
    if (!entry && !result) { /* Initialize the DirSearchInfo structure */
	dsinfo->nameptr = 0;
	return 1;
    }
    if (entry->attrs == 0)
	return -1; /* end of directory marker */
    if ((entry->attrs & FATTR_FILE) ||
	(entry->attrs & FATTR_DIRECTORY)) {
	dsinfo->nameptr = 0;
	result->attrs = entry->attrs;
	if (entry->attrs & FATTR_DELETED)
	    return 0;
	result->size = entry->size;
	result->firstblk = entry->firstblk;
	result->dirent = dirent;
	return entry->chunkcount ? 0 : 1;
    }
    if (entry->attrs & FATTR_DELETED)
	return 0;
    if (entry->attrs & FATTR_NAMECHUNK) {
	strncpy(&dsinfo->name[dsinfo->nameptr], (char*)&entry->chunkcount, 7);
	if (dsinfo->nameptr < sizeof(dsinfo->name) - 7)
	    dsinfo->nameptr += 7;
	return 0;
    }
    if (entry->attrs & FATTR_LASTCHUNK) {
	strncpy(&dsinfo->name[dsinfo->nameptr], &entry->chunkcount, 7);
	dsinfo->name[dsinfo->nameptr + 7] = NULL;
	result->fname = dsinfo->name;
	return (result->attrs & FATTR_DELETED) ? 0 : 1;
    }
    return 0;
}

static short int rootdir () {
    return divup(FSState.maxblocks * 2 + 16, FSState.blocksize);    
}

static FSFileInfo *find_dir_entry (unsigned short int block, char *entry) {
    FSDirEntry blockdata[FSState.blocksize / sizeof(FSDirEntry)];
    FSDirSearchInfo dsinfo;
    static FSFileInfo result;
    int readentries = 0;
    if (!entry)
	return &result;
    process_dir_entry(NULL, NULL, &dsinfo, (FSLocation){0, 0});
    read_block(block, &blockdata);
    check_error_ret(NULL);
    while (1) {
	switch (process_dir_entry(&blockdata[readentries], &result, &dsinfo, (FSLocation){block, readentries})) {
	    case -1:
		return NULL;
	    case 0:
		break;
	    case 1:
		if (!strcmp(result.fname, entry))
		    return &result;
	}
	if (++readentries == FSState.blocksize / sizeof(FSDirEntry)) {
	    block = read_fatentry(block);
	    if (block == 0)
		return NULL;
	    read_block(block, &blockdata);
	    check_error_ret(NULL);
	    readentries = 0;
	}
    }
}

static void add_dir_entry (block_t dir, FSFileInfo *inf) {
    int length = 1;
    FSLocation newdirent;
    FSDirEntry entries[FSState.blocksize / sizeof(FSDirEntry)];
    int entrynum = 1;
    if (inf->fname)
	length += divup(strlen(inf->fname), 7);
    if (length > 37) {
	fs_errno = FS_ENAMETOOLONG;
	return;
    }
    newdirent = find_dir_space(dir, length);
    read_block(newdirent.block, &entries);
    check_error();
    while (1) {
	if (entrynum == 1) {
	    entries[newdirent.offset].attrs = inf->attrs;
	    entries[newdirent.offset].chunkcount = length - 1;
	    entries[newdirent.offset].size = inf->size;
	    entries[newdirent.offset].firstblk = inf->firstblk;
	    inf->dirent = newdirent;
	} else {
	    if (entrynum == length)
	        entries[newdirent.offset].attrs = FATTR_LASTCHUNK;
	    else
	        entries[newdirent.offset].attrs = FATTR_NAMECHUNK;
	    strncpy(&entries[newdirent.offset].chunkcount, &inf->fname[(entrynum - 2) * 7], 7);
	    if (entrynum == length)
		break;
	}
	entrynum++;
	if (++newdirent.offset == FSState.blocksize / sizeof(FSDirEntry)) {
	    write_block(newdirent.block, &entries);
	    check_error();
	    newdirent.block = read_alloc_fatentry(newdirent.block, 1);
	    check_error();
	    read_block(newdirent.block, &entries);
	    check_error();
	    newdirent.offset = 0;
	}
    }
    write_block(newdirent.block, &entries);
}

static FSFileInfo *get_file_info (char *dir) {
    int len = strlen(dir);
    char tmp[len + 1], *ptr;
    int block;
    FSFileInfo *fi;
    while (*dir == '/') {
	dir ++;
	len --;
    }
    strcpy(tmp, dir);
    while (tmp[len?len-1:0] == '/' && len)
	tmp[--len] = NULL;
    if (!tmp[0]) {
	fi = find_dir_entry(0, NULL);
	fi->fname = "/";
	fi->size = 0;
	fi->attrs = FATTR_FILE | FATTR_DIRECTORY;
	fi->firstblk = rootdir();
	return fi;
    }
    if ((ptr = strrchr(tmp, '/'))) {
	*(ptr++) = NULL;
	fi = get_file_info(tmp);
	if (!fi) {
	    fs_errno = FS_ENOENT;
	    return NULL;
	}
	if (!(fi->attrs & FATTR_DIRECTORY)) {
	    fs_errno = FS_ENOTDIR;
	    return NULL;
	}
	block = fi->firstblk;
    } else {
	ptr = tmp;
	block = rootdir();
    }
    fi = find_dir_entry(block, ptr);
    if (fi)
	return fi;
    fs_errno = FS_ENOENT;
    return NULL;
}

static void file_update_dirent (FSFileInfo *f) {
    FSDirEntry entries[FSState.blocksize / sizeof(FSDirEntry)];
    read_block(f->dirent.block, &entries);
    check_error();
    entries[f->dirent.offset].firstblk = f->firstblk;
    entries[f->dirent.offset].size = f->size;
    write_block(f->dirent.block, &entries);
    check_error();
}

static void set_file_size (FSFile *f, int newsize) {
    FSFileInfo finfo;
    finfo.dirent = f->dirent;
    finfo.firstblk = f->first_block;
    finfo.size = newsize;
    f->file_size = newsize;
    file_update_dirent(&finfo);
}

static int create_dir_first_entry (FSFileInfo *dir) {
    block_t newblk = block_alloc();
    check_error_ret(0);
    zero_block(newblk);
    check_error_ret(0);
    dir->firstblk = newblk;
    dir->size = FSState.blocksize;
    file_update_dirent(dir);
    return newblk;
}

static FSFileInfo *create_file (char *pathname, int attrs) {
    FSFileInfo *result;
    int len = strlen(pathname);
    block_t dirblk;
    char tmp[len + 1], *ptr;
    memcpy(tmp, pathname, len+1);
    if ((ptr = strrchr(tmp, '/'))) {
        *(ptr++) = NULL;
	result = get_file_info(tmp);
	check_error_ret(NULL);
	dirblk = result->firstblk;
	if (!dirblk)
	    dirblk = create_dir_first_entry(result);
	check_error_ret(NULL);
    } else {
	result = find_dir_entry(0, NULL);
	ptr = pathname;
	dirblk = rootdir();
    }
    result->attrs = attrs;
    result->firstblk = 0;
    result->size = 0;
    result->fname = ptr;
    add_dir_entry(dirblk, result);
    return result;
}

static void dir_search_init (FSFileInfo *fi, FSDirSearchInfo *dsinfo) {
    process_dir_entry(NULL, NULL, dsinfo, (FSLocation){0, 0});
    dsinfo->dirent = (FSLocation){fi->firstblk, 0};
    dsinfo->cache = (FSDirEntry*)malloc(FSState.blocksize);
    if (!dsinfo->cache) {
	fs_errno = FS_ENOMEM;
	return;
    }
    read_block(dsinfo->dirent.block, dsinfo->cache);
    if (fs_errno) {
	free(dsinfo->cache);
	dsinfo->cache = 0;
    }
}

static void file_perform_seek (FSFile *f) {
    block_t blk;
    int i;
    if (f->seek_block == f->fileptr.block)
	return;
    if (f->seek_block < f->fileptr.block) {
	blk = f->first_block;
	i = 0;
    } else {
	blk = f->current_block;
	i = f->fileptr.block;
    }
    for (; i < f->seek_block; i++) {
	blk = read_fatentry(blk);
	if (!blk) {
	    fs_errno = FS_ENOBLOCK; /* XXX not suitable error */
	    return;
	}
	check_error();
    }
    f->current_block = blk;
    f->fileptr.block = f->seek_block;
}

/* exported functions */
int format_fs () {
    int i = 0;
    fs_errno = FS_NOERR;
    zero_block(rootdir());
    check_error_ret(-1);
    FSState.freeblocks = FSState.maxblocks - rootdir() - 1;
    FSState.freeblock = rootdir() + 1;
    set_fatentry(0, FSState.freeblock); 
    check_error_ret(-1);
    for (i = 1; i <= rootdir(); i++)
	set_fatentry(i, 0);		
    check_error_ret(-1);
    for (i = rootdir() + 1; i + 1 < FSState.maxblocks; i++)
	set_fatentry(i, i + 1);
    check_error_ret(-1);
    set_fatentry(FSState.maxblocks - 1, 0);
    check_error_ret(-1);
    flush_fat_cache();
    check_error_ret(-1);
    return 0;
}

int create_fsex (char *fname, block_t blocksize, block_t blockcount) {
    FSInfoBlock ib;
    fs_errno = FS_NOERR;
    ib.sig = 0x53465041;
    ib.version = 1;
    ib.blocksize = blocksize;
    ib.maxblocks = blockcount;
    FSState.maxblocks = ib.maxblocks;
    FSState.blocksize = ib.blocksize;
    ib.freeblocks = ib.maxblocks - rootdir() - 1;
    memset(ib.reserved, 0, sizeof(ib.reserved));
    FSState.fd = open(fname, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (FSState.fd < 0) {
	fs_errno = FS_EOS;
	return -1;
    }
    FSState.cachedfat = (short int*)malloc(FSState.blocksize);
    if (!FSState.cachedfat) {
	fs_errno = FS_ENOMEM;
	return -1;
    }
    FSState.cachedfat_block = 0;
    FSState.cachedfat_modified = 1;
    read_block(0, FSState.cachedfat);
    check_error_ret(-1);
    memcpy(FSState.cachedfat, &ib, sizeof(ib));
    return format_fs();
}

int create_fs(char *fname, unsigned int size) {
    int blocksize = 512;
    while (size / 65535 > blocksize)
        if (blocksize < 32768)
	    blocksize <<= 1;
	else 
    	    blocksize = 65535;
    return create_fsex(fname, blocksize, divup(size, blocksize));
}

int open_fs (char *fname) {
    FSInfoBlock ib;
    char data[512];
    fs_errno = FS_NOERR;
    FSState.fd = open(fname, O_RDWR);
    if (FSState.fd == -1) {
	fs_errno = FS_EOS;
	return -1;
    }
    FSState.blocksize = 512;
    FSState.maxblocks = 1;
    read_block(0, data);
    if (fs_errno) {
	close(FSState.fd);
	return -1;
    }
    memcpy(&ib, data, sizeof(ib));
    if (ib.sig != 0x53465041 || ib.blocksize < 16 || ib.maxblocks < 2 ||
	ib.blocksize % 8 != 0) {
	close(FSState.fd);
	fs_errno = FS_EFORMAT;
	return -1;
    }
    if (ib.version != 1) {
	close(FSState.fd);
	fs_errno = FS_EVERSION;
	return -1;
    }
    FSState.cachedfat_block = -1;
    FSState.cachedfat_modified = 0;
    FSState.cachedfat = (short int*)malloc(FSState.blocksize);
    if (!FSState.cachedfat) {
	fs_errno = FS_ENOMEM;
	return -1;
    }
    FSState.maxblocks = ib.maxblocks;
    FSState.blocksize = ib.blocksize;
    FSState.freeblocks = ib.freeblocks;
    FSState.freeblock = read_fatentry(0);
    return 0;
}

void close_fs () {
    fs_errno = FS_NOERR;
    flush_fat_cache();
    FSState.cachedfat_block = -1;
    free(FSState.cachedfat);
    check_os_error(close(FSState.fd));
}

FSInfo *fs_info () {
    static FSInfo result;
    fs_errno = FS_NOERR;
    result.blocksize = FSState.blocksize;
    result.freeblocks = FSState.freeblocks;
    result.totalblocks = FSState.maxblocks;
    return &result;
}

FSFile *fs_open (char *fname, int create) {
    FSFile *result;
    FSFileInfo *fi;
    fs_errno = FS_NOERR;
    fi = get_file_info(fname);
    if (fs_errno && fs_errno != FS_ENOENT)
	return NULL;
    if (!fi) {
	if (create) {
	    fs_errno = FS_NOERR;
	    fi = create_file(fname, FATTR_FILE);
	    check_error_ret(NULL);
	} else
	    return NULL;
    } else {
	if (fi->attrs & FATTR_DIRECTORY) {
	    fs_errno = FS_EISDIR;
	    return NULL;
	}
    }
    result = (FSFile*)malloc(sizeof(FSFile));
    if (!result) {
	fs_errno = FS_ENOMEM;
	return NULL;
    }
    result->first_block = fi->firstblk;
    result->current_block = fi->firstblk;
    result->seek_block = 0;
    result->file_size = fi->size;
    result->fileptr = (FSLocation) {0, 0};
    result->dirent = fi->dirent;
    return result;
}

typedef int (*funopen_read)(void *, char *, int);
typedef int (*funopen_write)(void *, const char *, int);
typedef fpos_t (*funopen_seek)(void *, fpos_t, int);
typedef int (*funopen_close)(void *);

#if USE_FUNOPEN
FILE *fs_fopen (char *name, char *mode) {
    int rd = 0, wr = 0, trunc = 0, append = 0;
    FSFile *f;
    if (!strcmp(mode, "r"))
	rd = 1;
    else if (!strcmp(mode, "r+")) {
	rd = 1;
	wr = 1;
    } else if (!strcmp(mode, "w"))
	wr = 1;
    else if (!strcmp(mode, "w+")) {
	wr = 1;
	rd = 1;
	trunc = 1;
    } else if (!strcmp(mode, "a")) {
	wr = 1;
	append = 1;
    } else if (!strcmp(mode, "a+")) {
	wr = 1;
	rd = 1;
	append = 1;
    }
    f = fs_open(name, trunc || append);
    check_error_ret(NULL);
    if (trunc) {
	fs_truncate(f);
        check_error_ret(NULL);
    }
    if (append) {
	fs_lseek(f, 0, SEEK_END);
        check_error_ret(NULL);
    }
    return funopen((void*)f, 
		    rd ? (funopen_read)fs_read : NULL, 
		    wr ? (funopen_write)fs_write : NULL,
		    (funopen_seek)fs_lseek, (funopen_close)fs_close);
}
#endif /* USE_FUNOPEN */
		    
int fs_close (FSFile *f) {
    free(f);
    return 0;
}

int fs_mkdir (char *path) {
    FSFileInfo *fi;
    fs_errno = FS_NOERR;
    fi = get_file_info(path);
    if (fs_errno && fs_errno != FS_ENOENT)
	return -1;
    if (fi) {
	fs_errno = FS_EEXIST;
	return -1;
    }
    fs_errno = FS_NOERR;
    create_file(path, FATTR_DIRECTORY);
    check_error_ret(-1);
    return 0;
}

FSFileInfo *fs_findfirst (char *directory, FSDirSearchInfo *dsinfo) {
    FSFileInfo *fi;
    fs_errno = FS_NOERR;
    fi = get_file_info(directory);
    check_error_ret(NULL);
    if ((fi->attrs & FATTR_DIRECTORY) != FATTR_DIRECTORY) {
	fs_errno = FS_ENOTDIR;
	return NULL;
    }
    if (!fi->firstblk)
	return NULL;
    dir_search_init(fi, dsinfo);
    check_error_ret(NULL);
    return fs_findnext(dsinfo);
}

FSFileInfo *fs_findnext (FSDirSearchInfo *dsinfo) {
    static FSFileInfo result;
    int found = 0;
    fs_errno = FS_NOERR;
    if (!dsinfo->cache)
	return NULL;
    while (1) {
	switch (process_dir_entry(&dsinfo->cache[dsinfo->dirent.offset], &result, dsinfo, dsinfo->dirent)) {
	    case -1:
		free(dsinfo->cache);
		dsinfo->cache = 0;
		return NULL;
	    case 1:
		found = 1;
	}
	if (++dsinfo->dirent.offset == FSState.blocksize / sizeof(FSDirEntry)) {
	    dsinfo->dirent.block = read_fatentry(dsinfo->dirent.block);
	    if (fs_errno) {
		free(dsinfo->cache);
		dsinfo->cache = 0;
		return NULL;
	    }
	    if (dsinfo->dirent.block == 0) {
		free(dsinfo->cache);
		dsinfo->cache = 0;
		if (found)
		    return &result;
		else
		    return NULL;
	    }
	    read_block(dsinfo->dirent.block, dsinfo->cache);
	    if (fs_errno) {
		free(dsinfo->cache);
		dsinfo->cache = 0;
		return NULL;
	    }
	    dsinfo->dirent.offset = 0;
	}
	if (found)
	    return &result;
    }
}

void fs_findend (FSDirSearchInfo *dsinfo) {
    if (dsinfo->cache)
	free(dsinfo->cache);
    dsinfo->cache = 0;
}

int fs_rmdir (char *path) {
    FSFileInfo *fi;
    FSDirSearchInfo dsinfo;
    fs_errno = FS_NOERR;
    fi = get_file_info(path);
    check_error_ret(-1);
    if ((fi->attrs & FATTR_DIRECTORY) != FATTR_DIRECTORY) {
	fs_errno = FS_ENOTDIR;
	return NULL;
    }
    if (fi->firstblk) {
	dir_search_init(fi, &dsinfo);
	check_error_ret(-1);
	if (fs_findnext(&dsinfo)) {
	    fs_findend(&dsinfo);
	    fs_errno = FS_ENOTEMPTY;
	    return -1;
	} else
	    check_error_ret(-1);
    }
    free_blocks(fi->firstblk);
    check_error_ret(-1);
    dir_free_ent(fi->dirent);
    check_error_ret(-1);
    return 0;
}

void fs_perror (char *string) {
    char *errors[] = {
	"Undefined error",
	"Operating system error",
	"No such file or directory",
	"File is not a directory",
	"File is a directory",
	"Filesystem out of space",
	"Name too long",
	"Block number is out of range",
	"File already exists",
	"Bad file format",
	"Invalid filesystem version",
	"Directory is not empty",
	"Out of memory"
    };
    int err;
    if (fs_errno >= sizeof(errors) / sizeof(errors[0]))
	err = 0;
    else
	err = fs_errno;
    if (err == 1) {
	if (string)
	    printf("%s: %s\n", string, strerror(errno));
    } else {
	if (string)
	    printf("%s: %s\n", string, errors[err]);
	else
	    printf("%s\n", errors[err]);
    }
}

int fs_remove (char *fname) {
    FSFileInfo *fi;
    fs_errno = FS_NOERR;
    fi = get_file_info(fname);
    check_error_ret(-1);
    if (fi->attrs & FATTR_DIRECTORY) {
	fs_errno = FS_EISDIR;
	return -1;
    }
    free_blocks(fi->firstblk);
    check_error_ret(-1);
    dir_free_ent(fi->dirent);
    check_error_ret(-1);
    return 0;
}

void fs_removef (FSFile *f) {
    fs_errno = FS_NOERR;
    free_blocks(f->first_block);
    check_error();
    dir_free_ent(f->dirent);
    check_error();
    fs_close(f);
}

/* The following things should be done: 
  1. resolve both filenames.
     If first is unresolvable, then ENOENT.
     If the second is resolvable, then filename = newpath, directory = second
     else try to resolve the path of the second. if not resolvable, return error,
     otherwise filename = what comes after /, directory = resolved entry.
  2.
  
  THIS CODE IS NOT READY YET !
*/
/*
int fs_move (char *name, char *newpath) {
    FSFileInfo *fi, *fi2;
    int len = strlen(newpath);
    char *newname, tmp[len + 1], *ptr;
    fs_errno = FS_NOERR;
    fi = get_file_info(name);
    check_error_ret(-1);
    fi2 = get_file_info(newname);
    if (fs_errno && fs_errno != FS_ENOENT)
	return -1;
    // XXX directory can't be moved inside itself
    if (fi2) {
	if ((fi2->attrs & FATTR_DIRECTORY) != FATTR_DIRECTORY) {
	    fs_errno = FS_EEXIST;
	    return -1;
	}
        strcpy(tmp, name);
	while(tmp[len?len-1:0] == '/' && len)
	    tmp[--len] = NULL;
	ptr = strrchr(tmp, "/");
	if (!ptr)
	    newname = tmp;
	else
	    newname = ptr + 1;
    } else {
        strcpy(tmp, newpath);
	while(tmp[len?len-1:0] == '/' && len)
	    tmp[--len] = NULL;
	ptr = strrchr(tmp, "/");
	if (ptr)
    }
}
*/
int fs_read (FSFile *f, void *buf, int count) {
    char blockdata[FSState.blocksize];
    int readcnt = 0, foffset, nextread;
    fs_errno = FS_NOERR;
    if (count < 1)
	return 0;
    file_perform_seek(f);
    check_error_ret(-1);
    while (1) {
	nextread = -1;
        foffset = f->fileptr.block * FSState.blocksize + f->fileptr.offset;
	if (f->file_size - foffset < FSState.blocksize || count - readcnt < FSState.blocksize) {
	    if (f->file_size - foffset < count - readcnt)
		nextread = f->file_size - foffset;
	    else
		nextread = count - readcnt;
	}
	if (!nextread)
	    break;
	if (nextread > 0 || f->fileptr.offset) {
	    int cnt = nextread > 0 && nextread < FSState.blocksize - f->fileptr.offset ? 
		      nextread : FSState.blocksize - f->fileptr.offset;
	    read_block(f->current_block, blockdata);
	    if (fs_errno)
		break;
	    memcpy((void*)((int)buf + readcnt), (void*)((int)blockdata + f->fileptr.offset), cnt);
	    readcnt += cnt;
	    f->fileptr.offset += cnt;
	    if (f->fileptr.offset >= FSState.blocksize) {
		f->fileptr.offset %= 512;
		f->seek_block++;
	    }
	} else {
	    read_block(f->current_block, (void*)((int)buf + readcnt));
	    if (fs_errno)
		break;
	    readcnt += FSState.blocksize;
	    f->seek_block++;
        }
	if (f->seek_block > f->fileptr.block) {
	    int nextblk = read_fatentry(f->current_block);
	    f->fileptr.block++;
	    if (!nextblk)	/* XXX should never happen */
		break;
	    f->current_block = nextblk; 
	}
    }
    return readcnt;
}

int fs_write (FSFile *f, void *buf, int count) {
    char blockdata[FSState.blocksize];
    int written = 0, 	/* how many bytes were written so far ? */
	newblocks = 0,  /* have we allocated new blocks ? */
	nextblk = 1;	/* holds the pointer to the next block of the file */
    fs_errno = FS_NOERR;
    if (count < 1)
	return 0;
    file_perform_seek(f);
    check_error_ret(-1);
    while (1) {
	if (f->fileptr.block >= divup(f->file_size, FSState.blocksize) && !newblocks) {
	    int newblock = allocate_blocks(divup(count - written, FSState.blocksize));
	    if (fs_errno)
		break;
	    if (f->current_block) {
		set_fatentry(f->current_block, newblock);
	    } else {
		f->first_block = newblock;
	    }
	    f->current_block = newblock;
	    newblocks = 1;
	}
	if (count == written)
	    break;
	if (count - written < FSState.blocksize || f->fileptr.offset) {
	    int cnt = count - written < FSState.blocksize - f->fileptr.offset ?
		      count - written : FSState.blocksize - f->fileptr.offset;
	    if (newblocks)
		memset(blockdata, 0, sizeof(blockdata));
	    else
		read_block(f->current_block, blockdata);
	    if (fs_errno)
		break;
	    memcpy((void*)((int)blockdata + f->fileptr.offset), (void*)((int)buf + written), cnt);
	    write_block(f->current_block, blockdata);
	    if (fs_errno)
		break;
	    written += cnt;
	    f->fileptr.offset += cnt;
	    if (f->fileptr.offset >= FSState.blocksize) {
		f->fileptr.offset %= 512;
		f->seek_block++;
	    }
	} else {
	    write_block(f->current_block, (void*)((int)buf + written));
	    if (fs_errno)
		break;
	    written += FSState.blocksize;
	    f->seek_block++;
        }
	if (f->seek_block > f->fileptr.block) {
	    nextblk = read_fatentry(f->current_block);
	    f->fileptr.block++;
	    if (nextblk) {
		f->current_block = nextblk; 
	    }
	}
    }
    if (f->fileptr.block * FSState.blocksize + f->fileptr.offset >= f->file_size)
	set_file_size(f, f->fileptr.block * FSState.blocksize + f->fileptr.offset);
    return written;
}

int fs_seek (FSFile *f, int offset) {
    if (offset > f->file_size)
	offset = f->file_size;
    else if (offset < 0)
	offset = 0;
    f->seek_block = offset / FSState.blocksize;
    f->fileptr.offset = offset % FSState.blocksize;
    return offset;
}

int fs_lseek (FSFile *f, int offset, int whence) {
    switch (whence) {
	case SEEK_CUR:
	    offset += f->fileptr.offset + f->seek_block * FSState.blocksize;
	    break;
	case SEEK_END:
	    offset += f->file_size; 
    }
    if (offset > f->file_size)
        offset = f->file_size;
    else if (offset < 0)
        offset = 0;
    f->seek_block = offset / FSState.blocksize;
    f->fileptr.offset = offset % FSState.blocksize;
    return offset;
}

int fs_tell (FSFile *f) {
    return f->seek_block * FSState.blocksize + f->fileptr.offset;
}

int fs_truncate (FSFile *f) {
    int nextblk;
    if (f->fileptr.block * FSState.blocksize + f->fileptr.offset >= f->file_size)
	return 0;
    fs_errno = FS_NOERR;
    nextblk = read_fatentry(f->current_block);
    free_blocks(nextblk);
    check_error_ret(-1);
    set_file_size(f, f->fileptr.block * FSState.blocksize + f->fileptr.offset);    
    return fs_errno ? -1 : 0;
}

int fs_getfilesize (FSFile *f) {
    return f->file_size;
}
