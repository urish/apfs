#include <sys/types.h>
#include <sys/stat.h>	 
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int intopic = 0, pre = 0, topics_found, datalen;
static char *datastart, *data;

void printattr (int bold, int reversed) {
    printf("\033[0");
    if (bold)
	printf(";1");
    if (reversed)
	printf(";7");
    printf("m");
}

int help_include(char *fname) {
    int fd = open(fname, O_RDONLY);
    struct stat sb;
    if (fd < 0) {
	printf("Unable to open \"%s\" for reading: %s\n", fname, strerror(errno));
	return 1;
    }
    fstat(fd, &sb);
    if (data) {
	int datadiff = (int)data - (int)datastart;
	datastart = (char*)realloc(datastart, datalen + sb.st_size + 1);
	data = (char*)((int)datastart + datadiff);
	memcpy((char*)((int)data + (int)sb.st_size - 1), data, strlen(data) + 1);
	read(fd, data, sb.st_size);
	datalen += sb.st_size;
    } else {
	data = (char*)malloc(sb.st_size + 1);
	if (!data) {
	    printf("Unable to allocate memory for helpfile.\n");
	    return 1;
	}
	read(fd, data, sb.st_size);
        data[sb.st_size] = 0;
	datalen = sb.st_size;
	datastart = data;
    }
    close(fd);
    return 0;
}

void process_tag (char *tag, char *args, int isend, char *reqtopic) {
    static int bold = 0, reversed = 0;
    if (!strcasecmp(tag, "TOPIC")) {
	char *name, *tmp;
	if (isend) {
	    intopic = 0;
	    return;
	}
	if (!reqtopic)
	    reqtopic = "";
	name = strstr(args, "name=") + 5;
	if (!name)
	    name = "";
	if (name[0] == '"') {
	    name++;
	    *strchr(name, '"') = NULL;
	} else {
	    tmp = name;
	    name = strsep(&tmp, " \t\n\r");
	}
	if (!strcasecmp(reqtopic, name)) {
	    printattr(bold, reversed);
	    intopic = 1;
	    topics_found++;
	}
    } else if (!strcasecmp(tag, "NOTOPIC")) {
	if (isend) {
	    intopic = 0;
	    return;
	} else if (!topics_found) {
	    printattr(bold, reversed);
	    intopic = 1;
	    topics_found++;
	}
    } else if (!strcasecmp(tag, "B")) {
	if (bold != isend)
	    return;
	bold = !isend;
        printattr(bold, reversed);
    } else if (!strcasecmp(tag, "R")) {
	if (reversed != isend)
	    return;
	reversed = !isend;
        printattr(bold, reversed);
    } else if (!strcasecmp(tag, "BR")) {
	if (!isend)
	    printf("\n");
    } else if (!strcasecmp(tag, "INCLUDE")) {
	if (isend)
	    return;
	data++;
	help_include(args);
	data--;
    } else if (!strcasecmp(tag, "PRE")) {
	pre = !isend;
    }
}

void showhelp (char *topic) {
    char *datastart, *tagn = 0, *taga = 0;
    int tagend = 0;
    int state = 0, brcount = 0, chcount = 0, tagcount = 0;
    /* state: 
      0 = plain text
      1 = tag name first letter
      2 = read tag name
      3 = first char of read tag args
      4 = read tag args
      5 = inside quotes
    */
    data = NULL;
    datastart = NULL;
    datalen = 0;
    topics_found = 0;
    if (help_include("test.hlp"))
	return;
    for (; *data; data++) {
        switch (state) {
	case 0:
	    if (*data == '<') {
		tagcount ++;
		state = 1;
	        tagend = 0;
	        tagn = data + 1;
		break;
	    }
	    if (!intopic)
	        break;
	    if (*data == '\n') {
		if (!chcount && tagcount)
		    break;
	        if (pre)
	            printf("\n");
	        else if (!brcount++)
	            printf(" ");
		chcount = 0;
		tagcount = 0;
	    } else {
	        brcount = 0;
		chcount++;
	        printf("%c", *data);
	    }
	    break;
	case 1:
	    switch (*data) {
	    case '\t':
	    case 10:
	    case 13:
	    case ' ':
		tagn++;
		break;
	    case '>':
		state = 0;
		break;
	    case '/':
		tagend = 1;
		tagn++;
	    	break;
	    default:
		state++;
	    }
	    break;
	case 2:
	    switch (*data) {
	    case '\t':
	    case 10:
	    case 13:
	    case ' ':
		*data = NULL;
		taga = data + 1;
		state++;
		break;
	    case '>':
		*data = NULL;
		process_tag(tagn, "", tagend, topic);
		state = 0;
		break;
	    }
	    break;
	case 3:
	    switch (*data) {
	    case '\t':
	    case 10:
	    case 13:
	    case ' ':
		taga++;
		break;
	    case '"':
	        state=5;
	        break;
	    case '>':
	        process_tag(tagn, "", tagend, topic);
	        state = 0;
		break;
	    default:
	        state++;
	    }
	    break;
	case 4:
	    switch (*data) {
	    case '"':
	        state++;
		break;
	    case '>':
	        *data = NULL;
	        process_tag(tagn, taga, tagend, topic);
	        state = 0;
	        break;
	    }
	    break;
	case 5:
	    if (*data == '"')
		state--;
	    break;
	}
    }
    free(datastart);
}
