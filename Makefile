CC=gcc
CFLAGS=-std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS=

BIN= manager baker cashier client
OBJ_COMMON=common.o

all: $(BIN)

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c -o common.o


manager: manager.c common.o common.h
	$(CC) $(CFLAGS) manager.c common.o -o manager $(LDFLAGS)

baker: baker.c common.o common.h
	$(CC) $(CFLAGS) baker.c common.o -o baker $(LDFLAGS)

cashier: cashier.c common.o common.h
	$(CC) $(CFLAGS) cashier.c common.o -o cashier $(LDFLAGS)

client: client.c common.o common.h
	$(CC) $(CFLAGS) client.c common.o -o client $(LDFLAGS)

clean:
	rm -f *.o $(BIN)
	rm -f .bakery_ipc_key bakery_ctrl.fifo

# Wyczyść zasoby IPC (użyj przed ponownym uruchomieniem jeśli poprzedni się nie zakończył poprawnie)
ipcclean:
	@echo "Czyszczenie zasobów IPC..."
	@ipcs -m | grep "$$(whoami | cut -c1-10)" | awk '{print $$2}' | while read id; do ipcrm -m $$id 2>/dev/null; done || true
	@ipcs -s | grep "$$(whoami | cut -c1-10)" | awk '{print $$2}' | while read id; do ipcrm -s $$id 2>/dev/null; done || true
	@ipcs -q | grep "$$(whoami | cut -c1-10)" | awk '{print $$2}' | while read id; do ipcrm -q $$id 2>/dev/null; done || true
	@rm -f .bakery_ipc_key bakery_ctrl.fifo
	@echo "Gotowe."

# Uruchom z czyszczeniem IPC
run: ipcclean
	./manager

# Uruchom test z czyszczeniem IPC
test: ipcclean
	./manager test 50

.PHONY: all clean ipcclean run test
