# APFS

A simple file system I wrote back in 2001, to prove a friend that implementing
a complete file system from scratch wasn't that hard.

The file system can be up to 2GB in size and hold up to 65,530 files and directories.

The basic operations are implemented: creating files and directories, reading/writing
files, deleting files, etc. Moving/renaming files is not implemented (there's some
preliminary code, but it's incomplete and commented out).

## Compiling

To compile the code on Ubuntu, you need to install the readline library:

```bash
sudo apt-get install libreadline-dev
```

In addition, you should disable the USE_FUNOPEN option in `apfs_config.h`:

```c
#define USE_FUNOPEN 0
```

Then run `make` to build the project. You will get a bunch of warnings,
but it will compile and build the test program. To run it, write `./test`.

Note: the code will not function correctly on 64-bit systems, as it assumes
`int` is large enough to hold a pointer. Back in 2001, personal computers had
128-256MB of RAM, and I don't think I even heard of 64-bit PCs at the time.
