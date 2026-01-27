#include "common.h"

/*
 * baker.c – proces piekarza: produkuje losowo produkty i dokłada na podajniki (FIFO).
 */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_evac = 0;
static void handler(int sig) {
    if (sig == SIG_EVAC) { g_evac = 1; g_stop = 1; }
    else if (sig == SIG_INV) {
        /* inwentaryzacja: nie zatrzymuje procesów */
        return;
    } else {
        g_stop = 1; /* SIGINT / SIGTERM */
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    install_signal_handlers_or_die(handler);

    /* Odszukaj IPC */
    ensure_ipc_key_file_or_die();
    IpcHandles h;
    memset(&h, 0, sizeof(h));

    h.shm_id = shmget(bakery_ftok_or_die(0x41), sizeof(BakeryState), IPC_PERMS_MIN);
    if (h.shm_id == -1) DIE_PERROR("shmget(baker)");

    h.sem_id = semget(bakery_ftok_or_die(0x42), 0, IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(baker)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(baker)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    int P = 0;
    shm_lock(h.sem_id);
    P = st->P;
    shm_unlock(h.sem_id);

    LOGF("piekarz", "Start pracy. Liczba produktów: %d", P);

    while (!g_stop) {
        /* Sprawdź czy sklep otwarty */
        shm_lock(h.sem_id);
        int open = st->store_open;
        int evacuated = st->evacuated;
        shm_unlock(h.sem_id);

        if (!open || evacuated) break;

        int wyprodukowano[MAX_P] = {0};
        /* Losowo wybierz ile produktów i ile sztuk do upieczenia */
        int batches = rand_between(1, 4);

        for (int b = 0; b < batches; ++b) {
            if (g_stop) break;
            int pid = rand_between(0, P - 1);
            int qty = rand_between(1, 5);

            for (int k = 0; k < qty; ++k) {
                if (g_stop) break;
                /* Czekaj na miejsce na podajniku pid */
                sem_P(h.sem_id, SEM_CONV_EMPTY(pid));
                sem_P(h.sem_id, SEM_CONV_MUTEX(pid));

                /* Sekcja krytyczna: dopisac na tail (FIFO) */
                Conveyor* cv = &st->conveyors[pid];
                
                /* Sprawdzenie poprawności capacity (Ki) – bezpieczeństwo przed dzieleniem modulo przez 0 */
                if (cv->capacity <= 0 || cv->capacity > MAX_KI) {
                    fprintf(stderr, "[baker] ERROR: invalid capacity=%d for product %d (MAX_KI=%d)\n", cv->capacity, pid, MAX_KI);
                    /* Cofnij zajęte miejsce i wyjdź bez zostawiania niespójności */
                    sem_V(h.sem_id, SEM_CONV_MUTEX(pid));
                    sem_V(h.sem_id, SEM_CONV_EMPTY(pid));
                    g_stop = 1;
                    break;
                }

                int pos = cv->tail;
                cv->items[pos] = 1; /* placeholder "sztuka" */
                cv->tail = (cv->tail + 1) % cv->capacity;
                cv->count++;

                /* Statystyka produkcji */
                st->produced[pid]++;
                wyprodukowano[pid]++;

                sem_V(h.sem_id, SEM_CONV_MUTEX(pid));
                sem_V(h.sem_id, SEM_CONV_FULL(pid));
            }
        }

        for (int i = 0; i < P; ++i) {
            if (wyprodukowano[i] > 0) {
                LOGF("piekarz", "Wypiek: %s x%d", st->produkty[i].nazwa, wyprodukowano[i]);
            }
        }
        /* TODO: czas między wypiekami */
        msleep(rand_between(200, 600));
    }

    /* TODO: jeśli ewakuacja – zakończ szybko */
    if (g_evac) LOGF("piekarz", "Kończę pracę (ewakuacja).");
    else        LOGF("piekarz", "Kończę pracę.");

    ipc_detach_or_die(st);
    return 0;
}
