#include "apfs_config.h"

typedef unsigned short int block_t;

typedef struct { /* total of 16 bytes */
    unsigned int sig; /* should be 0x55aaf00d*/
    short int version;
    block_t blocksize; /* in bytes */
    block_t maxblocks; /* maximum blocks in file system */
    block_t freeblocks; /* how many free blocks are in the file system */
    char reserved[4];
} FSInfoBlock;

#define FATTR_FILE	0x1
#define FATTR_NAMECHUNK	0x2
#define FATTR_LASTCHUNK 0x4
#define FATTR_DIRECTORY 0x8
#define FATTR_READONLY	0x40
#define FATTR_DELETED	0x80

typedef struct {
    char attrs;		/* byte 1 */
    char chunkcount; 	/* byte 2 */
    block_t firstblk; /* byte 3 - 4 */
    unsigned int size;  /* byte 5 - 8 */
} FSDirEntry;

typedef struct {
    block_t block, offset;
} FSLocation;

typedef struct {
    char attrs;
    char *fname;
    unsigned int size;
    block_t firstblk;
    FSLocation dirent;
} FSFileInfo;

/* physical block numbers: first_block, current_block
   logical block numbers: seek_block, fileptr.block */
typedef struct {
    int file_size;
    FSLocation fileptr, dirent;
    block_t first_block, current_block, seek_block;
} FSFile;

typedef struct {
    int totalblocks, freeblocks, blocksize;
} FSInfo;

typedef struct {
    int nameptr;
    char name[256];
    FSLocation dirent;
    FSDirEntry *cache;
} FSDirSearchInfo;

/* fs image manipulation functions */
extern int create_fs (char *, unsigned int);
extern int create_fsex (char *fname, block_t, block_t);
extern int open_fs (char *);
extern void close_fs (void);
extern void fs_flush (void);
extern FSInfo *fs_info (void);

/* directory functions */
extern int fs_mkdir (char *path);
extern int fs_rmdir (char *path);
extern FSFileInfo *fs_findfirst (char *, FSDirSearchInfo *);
extern FSFileInfo *fs_findnext (FSDirSearchInfo *);
extern void fs_findend (FSDirSearchInfo *);

/* functions to access files on the filesystem */
extern FSFile *fs_open (char*, int);	/* open/create a file */
#if USE_FUNOPEN
extern FILE *fs_fopen (char *, char *);
#endif /* USE_FUNOPEN */
extern int fs_close (FSFile *f);
extern int fs_read (FSFile *f, void *buf, int count);
extern int fs_write (FSFile *f, void *buf, int count);
extern int fs_seek (FSFile *f, int offset);
extern int fs_lseek (FSFile *f, int offset, int whence);
extern int fs_tell (FSFile *f);
extern int fs_truncate (FSFile *);
extern int fs_getfilesize (FSFile *);
extern void fs_removefi (FSFileInfo *);	/* delete a file specified by FSFileInfo */
extern int fs_remove (char *);		/* delete a file by its name */
extern void fs_removef (FSFile *f);	/* delete an open file */

/* fs_errno interface */
extern int fs_errno;
extern void fs_perror(char *str);

#define FS_NOERR	0	/* no error */
#define FS_EOS		1	/* operating system I/O error, see errno */
#define FS_ENOENT	2	/* No such file or directory */
#define FS_ENOTDIR	3	/* File is not a directory */
#define FS_EISDIR	4	/* Attempt to open/remove a directory */
#define FS_ENOSPACE	5	/* Out of space while allocating a block */
#define FS_ENAMETOOLONG	6	/* File name exceeded 252 chars */
#define FS_ENOBLOCK	7	/* Specified block number is out of range */
#define FS_EEXIST	8	/* File already exists */
#define FS_EFORMAT	9	/* Invalid filesystem format */
#define FS_EVERSION	10	/* Invalid version number */
#define FS_ENOTEMPTY	11	/* Directory not empty */
#define FS_ENOMEM	12	/* Out of memory */
