CC=gcc
CFLAGS=-std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS=

BIN=bakery manager baker cashier client
OBJ_COMMON=common.o

all: $(BIN)

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c -o common.o


manager: manager.c common.o common.h
	$(CC) $(CFLAGS) manager.c common.o -o manager $(LDFLAGS)

bakery: bakery.c common.o common.h
	$(CC) $(CFLAGS) bakery.c common.o -o bakery $(LDFLAGS)


baker: baker.c common.o common.h
	$(CC) $(CFLAGS) baker.c common.o -o baker $(LDFLAGS)

cashier: cashier.c common.o common.h
	$(CC) $(CFLAGS) cashier.c common.o -o cashier $(LDFLAGS)

client: client.c common.o common.h
	$(CC) $(CFLAGS) client.c common.o -o client $(LDFLAGS)

clean:
	rm -f *.o $(BIN)
	rm -f .bakery_ipc_key bakery_ctrl.fifo

.PHONY: all clean
