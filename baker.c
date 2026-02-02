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

    /* Faza rozgrzewki - wyprodukuj troche na zapas zanim klienci zaczna wchodzic */
    for (int warmup = 0; warmup < 3 && !g_stop; warmup++) {
        for (int pid = 0; pid < P && !g_stop; ++pid) {
            int qty = rand_between(2, 4);
            for (int k = 0; k < qty && !g_stop; ++k) {
                if (sem_P_nowait(h.sem_id, SEM_CONV_EMPTY(pid)) == -1) {
                    if (errno == EAGAIN) break; /* pelny podajnik */
                    continue;
                }
                sem_P(h.sem_id, SEM_CONV_MUTEX(pid));
                Conveyor* cv = &st->conveyors[pid];
                if (cv->capacity > 0 && cv->capacity <= MAX_KI) {
                    cv->items[cv->tail] = 1;
                    cv->tail = (cv->tail + 1) % cv->capacity;
                    cv->count++;
                    st->produced[pid]++;
                }
                sem_V(h.sem_id, SEM_CONV_MUTEX(pid));
                sem_V(h.sem_id, SEM_CONV_FULL(pid));
            }
        }
    }
    LOGF("piekarz", "Rozgrzewka zakonczona - produkty na polkach.");

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
                if (g_stop || g_evac) break;
                /* Czekaj na miejsce na podajniku pid */
                int empty_slots = semctl(h.sem_id, SEM_CONV_EMPTY(pid), GETVAL);
                if (empty_slots == 0) {
                    LOGF("piekarz", "Taśma pełna dla %s, czekam...", st->produkty[pid].nazwa);
                }
                
                /* Przerywalne oczekiwanie na semafor */
                struct sembuf sop = { .sem_num = SEM_CONV_EMPTY(pid), .sem_op = -1, .sem_flg = 0 };
                while (semop(h.sem_id, &sop, 1) == -1) {
                    if (errno == EINTR) {
                        if (g_stop || g_evac) goto cleanup;
                        continue;
                    }
                    perror("semop(baker EMPTY)");
                    goto cleanup;
                }
                
                if (g_stop || g_evac) {
                    /* Oddaj semafor i wyjdź */
                    sem_V(h.sem_id, SEM_CONV_EMPTY(pid));
                    goto cleanup;
                }
                
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
        msleep(rand_between(100, 300)); 
    }

cleanup:
    /* Inwentaryzacja: podsumowanie wytworzonych produktow */
    /* Wypisz raport zawsze przy zamknięciu sklepu lub ewakuacji */
    shm_lock(h.sem_id);
    int inv = st->inventory_mode;
    int closed = !st->store_open;
    int evac = st->evacuated;
    shm_unlock(h.sem_id);
    
    if (inv || closed || evac) {
        fprintf(stdout, "\n" COLOR_PIEKARZ);
        fprintf(stdout, "╔══════════════════════════════════════════════════════════╗\n");
        fprintf(stdout, "║       🥖 RAPORT PIEKARZA - WYPRODUKOWANE PRODUKTY        ║\n");
        fprintf(stdout, "╠══════════════════════════════════════════════════════════╣\n");
        fprintf(stdout, ANSI_RESET);
        
        int total = 0;
        shm_lock(h.sem_id);
        for (int i = 0; i < P; ++i) {
            int qty = st->produced[i];
            if (qty > 0) {
                fprintf(stdout, COLOR_PIEKARZ "║" ANSI_RESET "  P%02d: %-30s %6d szt.        " COLOR_PIEKARZ "║" ANSI_RESET "\n", 
                        i, st->produkty[i].nazwa, qty);
                total += qty;
            }
        }
        shm_unlock(h.sem_id);
        
        fprintf(stdout, COLOR_PIEKARZ "╠══════════════════════════════════════════════════════════╣" ANSI_RESET "\n");
        fprintf(stdout, COLOR_PIEKARZ "║" ANSI_RESET "  " ANSI_BOLD "SUMA WYPRODUKOWANYCH: %6d szt." ANSI_RESET "                       " COLOR_PIEKARZ "║" ANSI_RESET "\n", total);
        fprintf(stdout, COLOR_PIEKARZ "╚══════════════════════════════════════════════════════════╝" ANSI_RESET "\n");
    }

    if (g_evac) LOGF("piekarz", "Kończę pracę (ewakuacja).");
    else        LOGF("piekarz", "Kończę pracę.");

    ipc_detach_or_die(st);
    return 0;
}
