#include <stdio.h>
#include "apfs.h"

int main () {
    FSDirSearchInfo dsinfo;
    FSFileInfo *fi;
    FILE *f1, *f2;
    int rc;
    char buf[1024];
    printf("Reading archive... ");
    fflush(stdout);
    open_fs("apfs.24Mar2002.fs");
    if (fs_errno) {
	fs_perror("open_fs");
	return 0;
    }
    printf("OK\n");
    printf("Extracting files...\n");
    for (fi = fs_findfirst("/", &dsinfo); fi; fi = fs_findnext(&dsinfo)) {
	if ((fi->attrs & FATTR_DIRECTORY) != FATTR_DIRECTORY) {
	    f1 = fs_fopen(fi->fname, "r");
	    f2 = fopen(fi->fname, "w+");
	    while (!feof(f1)) {
		rc = fread(buf, 1, sizeof(buf), f1);
		fwrite(buf, rc, 1, f2);
	    }
	    fclose(f1);
	    fclose(f2);
	    printf("%s\n", fi->fname);
	}
    }
    if (fs_errno) {
	fs_perror("fs_findfirst");
	fs_findend(&dsinfo);
	return 1;
    }
    return 1;
}
