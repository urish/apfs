test: test.o apfs.o help.o
	gcc -o test test.o apfs.o help.o -lreadline

unpack: unpack.o apfs.o
	gcc -o unpack unpack.o apfs.o

.c.o: $<
	cc -O -pipe -c -Wall $<

clean:
	rm -f test *.o
