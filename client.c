#include "common.h"

/*
 * client.c – proces klienta:
 *  - czeka na wolne miejsce w sklepie (SEM_STORE_SLOTS)
 *  - robi zakupy: losuje listę min. 2 różne produkty, próbuje zdjąć z podajników FIFO
 *  - jeśli produkt niedostępny, nie kupuje
 *  - idzie do kasy i wysyła koszyk (msgsnd)
 *  - reaguje na ewakuację: przerywa i odkłada do kosza przy kasach (st->wasted[Pi])
 */

static volatile sig_atomic_t g_evac = 0;
static volatile sig_atomic_t g_stop = 0;

static void handler(int sig) {
    if (sig == SIG_EVAC) { g_evac = 1; g_stop = 1; }
    else if (sig == SIG_INV) {
        /* inwentaryzacja: nie zatrzymuje procesów */
        return;
    } else {
        g_stop = 1; /* SIGINT / SIGTERM */
    }
}

/* Wybór dwóch różnych produktów */
static void pick_two_distinct(int P, int* a, int* b) {
    *a = rand_between(0, P - 1);
    do { *b = rand_between(0, P - 1); } while (*b == *a);
}

static int choose_cashier(const BakeryState* st) {
    int best = -1;
    int best_len = 0x7fffffff;
    
    for (int i = 0; i < CASHIERS; ++i) {
        if (st->cashier_open[i] && st->cashier_accepting[i]) {
            int len = st->cashier_queue_len[i]; /* zakładamy, że pole istnieje w SHM */
            if (len < best_len) {
                best_len = len;
                best = i;
            }
        }
    }
    
    if (best != -1) {
        /* 2) jeśli kolejka > 2, spróbuj inną kasę accepting z kolejką <=2 */
        if (best_len > 2) {
            for (int i = 0; i < CASHIERS; ++i) {
                if (i == best) continue;
                if (st->cashier_open[i] && st->cashier_accepting[i]) {
                    int len = st->cashier_queue_len[i];
                    if (len <= 2) return i;
                }
            }
        }
        return best;
    }

    for (int i = 0; i < CASHIERS; ++i) {
        if (st->cashier_open[i]) return i;
    }
    return 0;
}

static void wait_before_store(int sem_id) {
    int said_waiting = 0;

    for (;;) {
        if (sem_P_nowait(sem_id, SEM_STORE_SLOTS) == 0) {
            if (said_waiting) {
                LOGF("klient", "Wchodzę do sklepu (zwolniło się miejsce).");
            }
            return; /* mamy “slot” w sklepie */
        }

        if (errno == EAGAIN) {
            if (!said_waiting) {
                LOGF("klient", "Czekam przed sklepem – brak wolnych miejsc.");
                said_waiting = 1;
            }
            msleep(200);
            continue;
        }

        if (errno == EINTR) continue;

        DIE_PERROR("sem_P_nowait(SEM_STORE_SLOTS)");
    }
}


int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    install_signal_handlers_or_die(handler);

    ensure_ipc_key_file_or_die();

    IpcHandles h;
    memset(&h, 0, sizeof(h));

    h.shm_id = shmget(bakery_ftok_or_die(0x41), sizeof(BakeryState), IPC_PERMS_MIN);
    if (h.shm_id == -1) DIE_PERROR("shmget(client)");

    h.sem_id = semget(bakery_ftok_or_die(0x42), 0, IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(client)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(client)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    /* Czy sklep jeszcze otwarty? */
    shm_lock(h.sem_id);
    int open = st->store_open;
    int P = st->P;
    shm_unlock(h.sem_id);

    if (!open) {
        ipc_detach_or_die(st);
        return 0;
    }

    /* Wejście do sklepu (limit N) */
    wait_before_store(h.sem_id);

    /* Zwiększ customers_in_store */
    shm_lock(h.sem_id);
    st->customers_in_store++;
    LOGF("klient", "Wchodzę do sklepu (klientów w sklepie: %d)", st->customers_in_store);
    shm_unlock(h.sem_id);

    /* czas wejścia/rozejrzenia się */
    msleep(rand_between(300, 800));

    /* Losowa lista zakupów: min 2 różne produkty, dołóż dodatkowe z pewnym prawdopodobieństwem */
    int want_count = 2 + (rand_between(0, 100) < 40 ? 1 : 0); /* 2 lub 3 */
    if (want_count > MAX_BASKET_ITEMS) want_count = MAX_BASKET_ITEMS;

    ClientMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.client_pid = getpid();

    /*  wybierz losowe produkty i ilości */
    int used[MAX_P] = {0};

    for (int i = 0; i < want_count; ++i) {
        /* poruszanie się po sklepie między podajnikami */
        msleep(rand_between(300, 800));
        if (g_stop) break;
        int pid;
        do { pid = rand_between(0, P - 1); } while (used[pid]);
        used[pid] = 1;

        int qty = rand_between(1, 4);

        /* Pobierz z podajnika, ale jeśli brak – nie kupuj */
        int bought = 0;
        for (int k = 0; k < qty; ++k) {
            /* czas na znalezienie produktu / sięgnięcie po kolejną sztukę */
            msleep(rand_between(300, 800));
            if (g_evac || g_stop) break;

            /* sprobwowac nowait na FULL */
            if (sem_P_nowait(h.sem_id, SEM_CONV_FULL(pid)) == -1) {
                if (errno == EAGAIN) {
                    /* brak towaru */
                    break;
                } else {
                    perror("semop NOWAIT FULL");
                    break;
                }
            }

            sem_P(h.sem_id, SEM_CONV_MUTEX(pid));

            /* Zdejmij z head (FIFO) */
            Conveyor* cv = &st->conveyors[pid];

            /* Sprawdzenie poprawności capacity (Ki) – bezpieczeństwo przed modulo przez 0 */
            if (cv->capacity <= 0 || cv->capacity > MAX_KI) {
                fprintf(stderr, "[client %d] ERROR: invalid capacity=%d for product %d (MAX_KI=%d)\n", (int)getpid(), cv->capacity, pid, MAX_KI);
                /* Cofnij pobranie z FULL (bo nie zdejmujemy faktycznie towaru) */
                sem_V(h.sem_id, SEM_CONV_MUTEX(pid));
                sem_V(h.sem_id, SEM_CONV_FULL(pid));
                g_stop = 1;
                break;
            }

            int removed = 0;

            if (cv->count > 0) {
                int pos = cv->head;
                (void)cv->items[pos]; /* placeholder */
                cv->items[pos] = 0;
                cv->head = (cv->head + 1) % cv->capacity;
                cv->count--;
                bought++;
                removed = 1;
            } else {
                fprintf(stderr,
                    "[client %d] WARN: inconsistency on pid=%d: FULL taken but count==0 (head=%d tail=%d cap=%d)\n",
                    (int)getpid(), pid, cv->head, cv->tail, cv->capacity);
                removed = 0;
            }

            sem_V(h.sem_id, SEM_CONV_MUTEX(pid));
            if (removed) {
                sem_V(h.sem_id, SEM_CONV_EMPTY(pid));
            } else {
                sem_V(h.sem_id, SEM_CONV_FULL(pid));
            }
            
        }

        if (bought > 0 && msg.item_count < MAX_BASKET_ITEMS) {
            msg.items[msg.item_count].product_id = pid;
            msg.items[msg.item_count].quantity = bought;
            msg.item_count++;
        }
    }

    /* Ewakuacja: odkładamy do kosza i wychodzimy */
    if (g_evac) {
        LOGF("klient", "EWAKUACJA! Odkładam towar do kosza i wychodzę.");
        shm_lock(h.sem_id); 
        LOGF("klient", "Zakończono zakupy, liczba pozycji w koszyku: %d", msg.item_count);
        for (int i = 0; i < msg.item_count; ++i) {
            int pid = msg.items[i].product_id;
            int qty = msg.items[i].quantity;
            if (pid >= 0 && pid < st->P && qty > 0) st->wasted[pid] += qty;
        }
        shm_unlock(h.sem_id);

        /* Wyjście */
        shm_lock(h.sem_id);
        st->customers_in_store--;
        shm_unlock(h.sem_id);
        sem_V(h.sem_id, SEM_STORE_SLOTS);

        ipc_detach_or_die(st);
        return 0;
    }

    /* Wybierz kasę i wyślij koszyk */
    int cashier = 0;
    shm_lock(h.sem_id);
    cashier = choose_cashier(st);
    shm_unlock(h.sem_id);

    /* Jeśli koszyk pusty, klient może iść prosto do wyjścia */
    if (msg.item_count > 0) {
        /* zanim wyśle, upewnij się że kasa nadal przyjmuje */
        shm_lock(h.sem_id);
        int ok = st->cashier_open[cashier] && st->cashier_accepting[cashier] && !st->evacuated && st->store_open;
        if (ok) st->cashier_queue_len[cashier]++;  /* klient "staje w kolejce" */
        shm_unlock(h.sem_id);

        if (ok) {
            fprintf(stdout, "[klient pid=%d] Wysyłam koszyk do kasy %d, item_count=%d\n",
                (int)getpid(), cashier, msg.item_count);
            if (msgsnd(h.msg_id[cashier], &msg, sizeof(ClientMsg) - sizeof(long), 0) == -1) {
                perror("msgsnd(client)");
                /* cofnij licznik kolejki jeśli się nie udało */
                shm_lock(h.sem_id);
                if (st->cashier_queue_len[cashier] > 0) st->cashier_queue_len[cashier]--;
                shm_unlock(h.sem_id);
                /* TODO: ewentualnie spróbować inną kasę */
            } else {
                LOGF("klient", "Wybrałem kasę %d (długość kolejki: %d)", cashier, st->cashier_queue_len[cashier]);
            }
        }
    }

    msleep(rand_between(50, 250));

    /* Wyjście */
    shm_lock(h.sem_id);
    st->customers_in_store--;
    shm_unlock(h.sem_id);

    sem_V(h.sem_id, SEM_STORE_SLOTS);

    ipc_detach_or_die(st);
    LOGF("klient", "Wychodzę ze sklepu.");
    return 0;
}
