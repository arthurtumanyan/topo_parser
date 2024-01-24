CC = gcc
CFLAGS  = -Wall -Wextra -std=c99
default: topo_parser

topo_parser:  main.o hash.o
	$(CC) $(CFLAGS) -o topo_parser hash.o main.o

#
main.o:  main.c
	$(CC) $(CFLAGS) -c main.c

#
hash.o:  hash.c
	$(CC) $(CFLAGS) -c hash.c

#
clean:
	$(RM) topo_parser *.o *~
