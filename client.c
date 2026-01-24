#include "common.h"

/*
 * client.c – proces klienta:
 *  - czeka na wolne miejsce w sklepie (SEM_STORE_SLOTS)
 *  - robi zakupy: losuje listę min. 2 różne produkty, próbuje zdjąć z podajników FIFO
 *  - jeśli produkt niedostępny, nie kupuje
 *  - idzie do kasy i wysyła koszyk (msgsnd)
 *  - reaguje na ewakuację: przerywa i odkłada do kosza przy kasach (st->wasted[Pi])
 *
 * TODO: dodać wybór kasy (np. najkrótsza kolejka, albo losowo spośród accepting).
 * TODO: dodać "czas zakupów" i poruszanie się.
 */

static volatile sig_atomic_t g_evac = 0;
static volatile sig_atomic_t g_stop = 0;

static void handler(int sig) {
    if (sig == SIG_EVAC) g_evac = 1;
    g_stop = 1;
}

/* Wybór dwóch różnych produktów */
static void pick_two_distinct(int P, int* a, int* b) {
    *a = rand_between(0, P - 1);
    do { *b = rand_between(0, P - 1); } while (*b == *a);
}

static int choose_cashier(const BakeryState* st) {
    /* TODO: inteligentny wybór kasy (np. queue_len), tu: pierwsza otwarta i accepting */
    for (int i = 0; i < CASHIERS; ++i) {
        if (st->cashier_open[i] && st->cashier_accepting[i]) return i;
    }
    /* Jeśli żadna accepting (np. w trakcie zamykania), idź do pierwszej otwartej */
    for (int i = 0; i < CASHIERS; ++i) {
        if (st->cashier_open[i]) return i;
    }
    return 0;
}

int main(void) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    install_signal_handlers_or_die(handler);

    ensure_ipc_key_file_or_die();

    IpcHandles h;
    memset(&h, 0, sizeof(h));

    h.shm_id = shmget(bakery_ftok_or_die(0x41), sizeof(BakeryState), IPC_PERMS_MIN);
    if (h.shm_id == -1) DIE_PERROR("shmget(client)");

    h.sem_id = semget(bakery_ftok_or_die(0x42), sem_count_for_P(MAX_P), IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(client)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(client)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    /* Czy sklep jeszcze otwarty? */
    shm_lock_or_die(h.sem_id);
    int open = st->store_open;
    int P = st->P;
    shm_unlock_or_die(h.sem_id);

    if (!open) {
        ipc_detach_or_die(st);
        return 0;
    }

    /* Wejście do sklepu (limit N) */
    sem_P_or_die(h.sem_id, SEM_STORE_SLOTS);

    /* Zwiększ customers_in_store */
    shm_lock_or_die(h.sem_id);
    st->customers_in_store++;
    shm_unlock_or_die(h.sem_id);

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
        int pid;
        do { pid = rand_between(0, P - 1); } while (used[pid]);
        used[pid] = 1;

        int qty = rand_between(1, 4);

        /* Pobierz z podajnika, ale jeśli brak – nie kupuj */
        int bought = 0;
        for (int k = 0; k < qty; ++k) {
            if (g_evac) break;

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

            sem_P_or_die(h.sem_id, SEM_CONV_MUTEX(pid));

            /* Zdejmij z head (FIFO) */
            Conveyor* cv = &st->conveyors[pid];
            if (cv->count > 0) {
                int pos = cv->head % MAX_KI;
                (void)cv->items[pos]; /* placeholder */
                cv->items[pos] = 0;
                cv->head = (cv->head + 1) % cv->capacity;
                cv->count--;
                bought++;
            } else {
                /* niespójność: FULL wskazał, a count==0 */
                /* TODO: rozwiązac spójność (powinna nie wystąpić) */
            }

            sem_V_or_die(h.sem_id, SEM_CONV_MUTEX(pid));
            sem_V_or_die(h.sem_id, SEM_CONV_EMPTY(pid));
        }

        if (bought > 0 && msg.item_count < MAX_BASKET_ITEMS) {
            msg.items[msg.item_count].product_id = pid;
            msg.items[msg.item_count].quantity = bought;
            msg.item_count++;
        }
    }

    /* Ewakuacja: odkładamy do kosza i wychodzimy */
    if (g_evac) {
        shm_lock_or_die(h.sem_id);
        for (int i = 0; i < msg.item_count; ++i) {
            int pid = msg.items[i].product_id;
            int qty = msg.items[i].quantity;
            if (pid >= 0 && pid < st->P && qty > 0) st->wasted[pid] += qty;
        }
        shm_unlock_or_die(h.sem_id);

        /* Wyjście */
        shm_lock_or_die(h.sem_id);
        st->customers_in_store--;
        shm_unlock_or_die(h.sem_id);
        sem_V_or_die(h.sem_id, SEM_STORE_SLOTS);

        ipc_detach_or_die(st);
        return 0;
    }

    /* Wybierz kasę i wyślij koszyk */
    int cashier = 0;
    shm_lock_or_die(h.sem_id);
    cashier = choose_cashier(st);
    shm_unlock_or_die(h.sem_id);

    /* Jeśli koszyk pusty, klient może iść prosto do wyjścia */
    if (msg.item_count > 0) {
        if (msgsnd(h.msg_id[cashier], &msg, sizeof(ClientMsg) - sizeof(long), 0) == -1) {
            perror("msgsnd(client)");
            /* TODO: decyzja – w razie błędu: wyjść czy próbować inną kasę */
        }
    }

    /* TODO: symuluj czas stania w kolejce (w praktyce kasjer blokuje na msgrcv, klient po wysłaniu może wyjść) */
    msleep(rand_between(50, 250));

    /* Wyjście */
    shm_lock_or_die(h.sem_id);
    st->customers_in_store--;
    shm_unlock_or_die(h.sem_id);

    sem_V_or_die(h.sem_id, SEM_STORE_SLOTS);

    ipc_detach_or_die(st);
    return 0;
}
