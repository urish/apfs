#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "apfs.h"

#define check_fs_open() 	\
	if (!fsopen) {		\
	    printf("Error: no filesystem is currently open.\n");	\
	    return;		\
	}

int fsopen = 0;

void cmd_exit (char *args) {
    if (fsopen)
	close_fs();
    printf("Bye Bye !\n");
    exit(1);
}

void cmd_create (char *args) {
    char *name;
    int size;
    name = strsep(&args, " \t");
    if (!args) {
	printf("No size specified.\n");
	return;
    }
    size = atol(strsep(&args, " \t"));
    if (size % 8 != 0 || size < 16) {
	printf("Bad size specification: size must be a mupltiplication of 8, at least 16 bytes.\n");
	return;
    }
    if (args) {
	int blocks = atol(args);
	if (blocks < 2) {
	    printf("Bad blocks specification: there must be at least 2 block in a filesystem.\n");
	    return;
	}
	if (fsopen)
	    close_fs();
	if (create_fsex(name, size, blocks))
	    fs_perror("fs_create");
	else
	    fsopen = 1;
    } else {
	if (fsopen)
	    close_fs();
	if (create_fs(name, size))
	    fs_perror("fs_create");
	else
	    fsopen = 1;
    }
}

void cmd_open (char *args) {
    if (fsopen)
	close_fs();
    if (open_fs(args))
	fs_perror("fs_open");
    else
	fsopen = 1;
}

void cmd_close (char *args) {
    check_fs_open();
    close_fs();
    fsopen = 0;
}

void cmd_info (char *args) {
    FSInfo *inf;
    int convert;
    check_fs_open();
    inf = fs_info();
    convert = 1048576 / inf->blocksize;
    printf("File system information:\n");
    printf("Block size: %d bytes.\n", inf->blocksize);
    printf("Total %d blocks, %d free.\n", inf->totalblocks, inf->freeblocks);
    printf("Total %.2fMB, %.2fMB used (%.1f%%), %.2fMB free.\n", 
	    (float)inf->totalblocks / (float)convert, 
	    (float)(inf->totalblocks - inf->freeblocks) / (float)convert,
	    (float)(inf->totalblocks - inf->freeblocks) * 100.0 / 
	    (float)inf->totalblocks, (float)inf->freeblocks / (float)convert);
}

void cmd_mkdir (char *args) {
    check_fs_open();
    if (fs_mkdir(args))
	fs_perror("fs_mkdir");
}

void cmd_rmdir (char *args) {
    check_fs_open();
    if (fs_rmdir(args))
	fs_perror("fs_rmdir");
}

void cmd_ls (char *args) {
    FSDirSearchInfo dsinfo;
    FSFileInfo *fi;
    check_fs_open();
    printf("Directory listing for %s:\n", *args ? args : "/");
    for (fi = fs_findfirst(args, &dsinfo); fi; fi = fs_findnext(&dsinfo)) {
	if ((fi->attrs & FATTR_DIRECTORY) == FATTR_DIRECTORY) {
	    printf("%-40s <DIR>\n", fi->fname);
	} else {
	    printf("%-40s %d\n", fi->fname, fi->size);
	}
    }
    if (fs_errno) {
	fs_perror("fs_findfirst");
	fs_findend(&dsinfo);
    }
}

void cmd_copyin (char *args) {
    char *name = strsep(&args, " \t");
    FSFile *f;
    int rc, fd;
    char buf[4096];
    check_fs_open();
    if (!args) {
	printf("No target filename specified.\n");
	return;
    }
    fd = open(name, O_RDONLY);
    if (fd < 0) {
	perror("open");
	return;
    }
    f = fs_open(args, 1);
    if (!f) {
	fs_perror("fs_open");
	return;
    }
    while ((rc = read(fd, buf, sizeof(buf))))
	if (fs_write(f, buf, rc) != rc) {
	    fs_perror("fs_write");
	    return;
	}
    fs_truncate(f);
    close(fd);
    fs_close(f);
}

void cmd_copyout (char *args) {
    char *name = strsep(&args, " \t");
    FSFile *f;
    int rc, fd;
    char buf[4096];
    check_fs_open();
    if (!args) {
	printf("No target filename specified.\n");
	return;
    }
    f = fs_open(name, 0);
    if (!f) {
	fs_perror("fs_open");
	return;
    }
    fd = open(args, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
	perror("open");
	return;
    }
    while ((rc = fs_read(f, buf, sizeof(buf))))
	if (write(fd, buf, rc) != rc) {
	    perror("write");
	    return;
	}
    fs_close(f);
    close(fd);
}

void cmd_del (char *args) {
    check_fs_open();
    if (fs_remove(args))
	fs_perror("fs_remove");
}

#define elif else if

extern void showhelp(char*);

void process_command (char *line) {
    char *cmd, *args;
    cmd = strsep(&line, " \t");
    if (line)
	args = line;
    else
	args = "";
    if (!strcmp(cmd, "help"))
	showhelp(args);
    elif (!strcmp(cmd, "exit"))
	cmd_close(args);
    elif (!strcmp(cmd, "open"))
	cmd_open(args);
    elif (!strcmp(cmd, "create"))
	cmd_create(args);
    elif (!strcmp(cmd, "close"))
	cmd_close(args);
    elif (!strcmp(cmd, "info"))
	cmd_info(args);
    elif (!strcmp(cmd, "mkdir"))
	cmd_mkdir(args);
    elif (!strcmp(cmd, "rmdir"))
	cmd_rmdir(args);
    elif (!strcmp(cmd, "ls"))
	cmd_ls(args);
    elif (!strcmp(cmd, "copyin"))
	cmd_copyin(args);
    elif (!strcmp(cmd, "copyout"))
	cmd_copyout(args);
    elif (!strcmp(cmd, "del"))
	cmd_del(args);
    else
	printf("Invalid command name \"%s\".\nFor more information, type \"help\".\n", cmd);
}

int main (int argc, char *argv[]) {
    char *line, *linestart;
    signal(SIGINT, (sig_t)cmd_exit);
    if (argc > 1) {
        int i;
	for (i = 1; i < argc; i++)
	    process_command(argv[i]);
	close(1);
	cmd_exit(0);
    }
    while (1) {
	line = readline("apfs> ");
	if (!line)
	    continue;
	linestart = line;
	while ((*line == ' ' || *line == '\t') && *line)
	    line++;
	if (!*line)
	    continue;
	add_history(line);
	process_command(line);
	free(linestart);
    }
    return 0;
}
