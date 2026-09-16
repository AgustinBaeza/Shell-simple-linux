CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11

mishell: mishell.c
	$(CC) $(CFLAGS) -o mishell mishell.c

clean:
	rm -f mishell