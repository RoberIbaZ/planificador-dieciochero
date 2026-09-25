CC = gcc
CFLAGS = -Wall -Wextra -std=c17

planificador: planificador.c
	$(CC) $(CFLAGS) -o planificador planificador.c

clean:
	rm -f planificador
