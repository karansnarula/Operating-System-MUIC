CC=gcc
CFLAGS=-Wall -w -pedantic -g

SRC = icsh.c
BIN = icsh

all: exec

exec: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) -lreadline

clean:
	$(RM) $(BIN)