#ifndef BAKERY_COMMON_H
#define BAKERY_COMMON_H

/*
 * Wspólne definicje dla: bakery (kierownik), baker (piekarz), cashier (kasjer), client (klient).
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* =========================
 *  Konfiguracja projektu
 * ========================= */

#define PROJECT_NAME        "bakery"
#define IPC_KEY_FILE        "./.bakery_ipc_key"   /* Tworzony przez bakery, używany do ftok() */
#define CTRL_FIFO_PATH      "./bakery_ctrl.fifo"  /* Opcjonalny kanał sterowania */

/* Minimalne prawa dostępu*/
#define IPC_PERMS_MIN       0600
#define FIFO_PERMS_MIN      0600

/* Ograniczenia statyczne*/
#define MAX_P               15      
#define MAX_KI              64     

#define CASHIERS            3

/* Sygnały*/
#define SIG_EVAC            SIGUSR1
#define SIG_INV             SIGUSR2

/* =========================
 *  Logowanie / błędy
 * ========================= */

#define DIE_PERROR(msg) do { \
    perror(msg);            \
    exit(EXIT_FAILURE);     \
} while (0)

#define CHECK_SYS(call, msg) do { \
    if ((call) == -1) {           \
        perror(msg);              \
        exit(EXIT_FAILURE);       \
    }                             \
} while (0)

#define CHECK_PTR(call, msg) do { \
    if ((call) == (void*)-1 || (call) == NULL) { \
        perror(msg);                               \
        exit(EXIT_FAILURE);                        \
    }                                              \
} while (0)

#define LOGF(tag, ...) do { \
    fprintf(stdout, "[%s pid=%d] ", (tag), (int)getpid()); \
    fprintf(stdout, __VA_ARGS__); \
    fprintf(stdout, "\n"); \
} while (0)
/* =========================
 *  Struktury danych w SHM
 * ========================= */

/* Podajnik FIFO (bufor cykliczny) dla jednego produktu. */
typedef struct Conveyor {
    int capacity;                 /* Ki */
    int head;                     /* indeks odczytu */
    int tail;                     /* indeks zapisu */
    int count;                    /* liczba sztuk na podajniku */
    int items[MAX_KI];            
} Conveyor;

typedef struct Product {
    char nazwa[64];            /* nazwa produktu */
    double cena;              /* cena produktu */
} Product;

/* Konfiguracja i stan globalny */
typedef struct BakeryState {
    int P;                        /* liczba produktów*/
    int N;                        /* max klientów w sklepie */
    int open_hour;                /* Tp */
    int close_hour;               /* Tk */

    Product produkty[MAX_P];      /* lista produktów: nazwa + cena */
    int Ki[MAX_P];                /* pojemność podajnika i */

    /* Stan */
    int store_open;               /* 1=otwarty, 0=zamykanie/zamknięty */
    int inventory_mode;           /* 1 po SIG_INV */
    int evacuated;                /* 1 po SIG_EVAC */

    int customers_in_store;       /* aktualna liczba klientów */

    int cashier_open[CASHIERS];       /* czy kasa jest otwarta */
    int cashier_accepting[CASHIERS];  /* czy kasa przyjmuje nowych (zamykanie = 0) */
    int cashier_queue_len[CASHIERS];

    /* Statystyki */
    int produced[MAX_P];          /* ile wyprodukowano (sumarycznie) */
    int wasted[MAX_P];            /* ile wyrzucono do kosza (ewakuacja) */
    int sold_by_cashier[CASHIERS][MAX_P]; /* ile skasował każdy kasjer */

    Conveyor conveyors[MAX_P];    /* FIFO dla każdego produktu */

    /* TODO: queue_len[3] do wyboru najmniejszej kolejki */
    /* TODO: dodatkowe liczniki/metryki do sprawozdania */
} BakeryState;

/* =========================
 *  Indeksy semaforów
 * ========================= */

/*
 * Używamy jednego zestawu semaforów (semget) i mapujemy indeksy:
 *  - SEM_STORE_SLOTS: licznik wolnych miejsc w sklepie (N)
 *  - SEM_SHM_GLOBAL : mutex na pola globalne w SHM
 *  - Dla każdego produktu i:
 *      SEM_CONV_MUTEX(i)  : mutex na conveyor i
 *      SEM_CONV_EMPTY(i)  : licznik wolnych miejsc (Ki)
 *      SEM_CONV_FULL(i)   : licznik sztuk dostępnych
 *
 * TODO: Przeanalizowac czy potrzeba osobnych mutexów dla kas, kolejek itp.
 */

#define SEM_STORE_SLOTS     0
#define SEM_SHM_GLOBAL      1

/* Początek semaforów per produkt */
#define SEM_PRODUCTS_BASE   2
#define SEM_PER_PRODUCT     3

#define SEM_CONV_MUTEX(i)   (SEM_PRODUCTS_BASE + (i) * SEM_PER_PRODUCT + 0)
#define SEM_CONV_EMPTY(i)   (SEM_PRODUCTS_BASE + (i) * SEM_PER_PRODUCT + 1)
#define SEM_CONV_FULL(i)    (SEM_PRODUCTS_BASE + (i) * SEM_PER_PRODUCT + 2)

/* Całkowita liczba semaforów w zestawie: 2 + 3*P */
static inline int sem_count_for_P(int P) { return SEM_PRODUCTS_BASE + SEM_PER_PRODUCT * P; }

/* =========================
 *  Kolejki komunikatów
 * ========================= */

#define MAX_BASKET_ITEMS    16  

typedef struct BasketItem {
    int product_id;
    int quantity;
} BasketItem;

/* Wiadomość klient -> kasjer (System V: musi zaczynać się od long mtype) */
typedef struct ClientMsg {
    long mtype;              /* np. 1 */
    pid_t client_pid;
    int item_count;
    BasketItem items[MAX_BASKET_ITEMS];
    /* TODO: można dodać sumę, timestamp, itp. */
} ClientMsg;

/* =========================
 *  Uchwyt do zasobów IPC
 * ========================= */

typedef struct IpcHandles {
    int shm_id;
    int sem_id;
    int msg_id[CASHIERS];
} IpcHandles;

/* =========================
 *  API wspólne (common.c)
 * ========================= */

#ifdef __cplusplus
extern "C" {
#endif

/* Inicjalizacja / podłączenie */
key_t bakery_ftok_or_die(int proj_id);
void ensure_ipc_key_file_or_die(void);

void ipc_create_or_die(IpcHandles* out, int P);
void ipc_attach_or_die(const IpcHandles* h, BakeryState** out_state);
void ipc_detach_or_die(BakeryState* state);
void ipc_destroy_or_die(const IpcHandles* h, int P);

/* Semafory: operacje P/V + nowait */
void sem_P(int sem_id, int sem_num);
int  sem_P_nowait(int sem_id, int sem_num); /* 0=ok, -1=błąd (errno ustawione) */
void sem_V(int sem_id, int sem_num);

/* Mutex dla SHM globalnej */
void shm_lock(int sem_id);
void shm_unlock(int sem_id);

/* Losowanie */
int rand_between(int a, int b);

/* Walidacja parametrów (bakery) */
int validate_config(int P, int N, int open_hour, int close_hour, const int* Ki, const Product* produkty);

/* Bezpieczna instalacja handlerów sygnałów */
void install_signal_handlers_or_die(void (*handler)(int));

/* Pomocnicze: czas */
void msleep(int ms);

#ifdef __cplusplus
}
#endif

/* System V semctl wymaga union semun (nie zawsze zdefiniowane) */
union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

#endif /* BAKERY_COMMON_H */
