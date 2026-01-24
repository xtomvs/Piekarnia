#include "common.h"

/*
 * baker.c – proces piekarza: produkuje losowo produkty i dokłada na podajniki (FIFO).
 *
 * TODO: Produkcja "co określony czas", losowa liczba sztuk różnych produktów.
 * TODO: Aktualizuj st->produced[Pi].
 * TODO: Reaguj na SIG_EVAC / SIGINT / SIGTERM.
 */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_evac = 0;
static void handler(int sig) {
    if (sig == SIG_EVAC) g_evac = 1;
    g_stop = 1;
}

int main(void) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    install_signal_handlers_or_die(handler);

    /* Odszukaj IPC */
    ensure_ipc_key_file_or_die();
    IpcHandles h;
    memset(&h, 0, sizeof(h));

    h.shm_id = shmget(bakery_ftok_or_die(0x41), sizeof(BakeryState), IPC_PERMS_MIN);
    if (h.shm_id == -1) DIE_PERROR("shmget(baker)");

    int P_guess = MAX_P; /* TODO: jeśli chcesz, odczytaj P po attach */
    h.sem_id = semget(bakery_ftok_or_die(0x42), sem_count_for_P(P_guess), IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(baker)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(baker)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    int P = 0;
    shm_lock_or_die(h.sem_id);
    P = st->P;
    shm_unlock_or_die(h.sem_id);

    while (!g_stop) {
        /* Sprawdź czy sklep otwarty */
        shm_lock_or_die(h.sem_id);
        int open = st->store_open;
        int evacuated = st->evacuated;
        shm_unlock_or_die(h.sem_id);

        if (!open || evacuated) break;

        /* Losowo wybierz ile produktów i ile sztuk do upieczenia */
        int batches = rand_between(1, 4);

        for (int b = 0; b < batches; ++b) {
            int pid = rand_between(0, P - 1);
            int qty = rand_between(1, 5);

            for (int k = 0; k < qty; ++k) {
                /* Czekaj na miejsce na podajniku pid */
                sem_P_or_die(h.sem_id, SEM_CONV_EMPTY(pid));
                sem_P_or_die(h.sem_id, SEM_CONV_MUTEX(pid));

                /* Sekcja krytyczna: dopisac na tail (FIFO) */
                Conveyor* cv = &st->conveyors[pid];
                /* TODO: dopilnowac capacity != 0 */
                int pos = cv->tail % MAX_KI;
                cv->items[pos] = 1; /* placeholder "sztuka" */
                cv->tail = (cv->tail + 1) % cv->capacity;
                cv->count++;

                /* Statystyka produkcji */
                st->produced[pid]++;

                sem_V_or_die(h.sem_id, SEM_CONV_MUTEX(pid));
                sem_V_or_die(h.sem_id, SEM_CONV_FULL(pid));
            }
        }

        /* TODO: czas między wypiekami */
        msleep(rand_between(200, 600));
    }

    /* TODO: jeśli ewakuacja – zakończ szybko */
    (void)g_evac;

    ipc_detach_or_die(st);
    return 0;
}
