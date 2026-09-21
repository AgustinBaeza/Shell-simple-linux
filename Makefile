CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11

miShell: miShell.c
	$(CC) $(CFLAGS) -o miShell miShell.c

clean:
	rm -f miShell