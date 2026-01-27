#include "common.h"

/*
 * cashier.c – proces kasjera:
 *  - odbiera wiadomości klientów z kolejki przypisanej do tej kasy
 *  - aktualizuje sold_by_cashier[cashier_id][Pi]
 *  - reaguje na zamykanie kasy: cashier_accepting=0 -> nie przyjmuje nowych, ale obsługuje kolejkę
 *
 * TODO: "paragon" – wypisywanie lub logowanie transakcji.
 * TODO: inwentaryzacja po zamknięciu sklepu: wydruk podsumowania.
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

static void print_summary(const BakeryState* st, int cashier_id) {
    /* TODO:  raport */
    fprintf(stdout, "\n=== PODSUMOWANIE KASY %d ===\n", cashier_id);
    for (int i = 0; i < st->P; ++i) {
        int s = st->sold_by_cashier[cashier_id][i];
        if (s > 0) fprintf(stdout, "Produkt %d: %d szt.\n", i, s);
    }
}

static void process_sale(BakeryState* st, int sem_id, int cashier_id, const ClientMsg* msg) {
    /* Księgowanie zakupów kasjera (sztuki per produkt) */
    fprintf(stdout,
        "[kasjer pid=%d] KASUJĘ: klient_pid=%d, pozycji=%d (kasa=%d)\n",
        (int)getpid(), (int)msg->client_pid, msg->item_count, cashier_id);
    shm_lock(sem_id);
    for (int i = 0; i < msg->item_count; ++i) {
        int pid = msg->items[i].product_id;
        int qty = msg->items[i].quantity;
        if (pid >= 0 && pid < st->P && qty > 0) {
            st->sold_by_cashier[cashier_id][pid] += qty;
        }
        /* TODO: pełna walidacja komunikatu (item_count range, qty range) */
    }
    shm_unlock(sem_id);

    /* TODO: "wydruk paragonu" z nazwami i cenami: st->produkty[pid].nazwa, st->produkty[pid].cena */
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "Użycie: cashier <id 0..2>\n");
        return EXIT_FAILURE;
    }
    int cashier_id = atoi(argv[1]);
    if (cashier_id < 0 || cashier_id >= CASHIERS) {
        fprintf(stderr, "Błędny id kasjera.\n");
        return EXIT_FAILURE;
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    install_signal_handlers_or_die(handler);

    ensure_ipc_key_file_or_die();
    IpcHandles h;
    memset(&h, 0, sizeof(h));

    h.shm_id = shmget(bakery_ftok_or_die(0x41), sizeof(BakeryState), IPC_PERMS_MIN);
    if (h.shm_id == -1) DIE_PERROR("shmget(cashier)");

    
    h.sem_id = semget(bakery_ftok_or_die(0x42), 0, IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(cashier)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(cashier)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    LOGF("kasjer", "Start pracy. Stanowisko: %d", cashier_id);

    int prev_store_open = -1, prev_opened = -1, prev_accepting = -1, prev_evacuated = -1;
    int said_not_accepting = 0;

    while (!g_stop) {
        /* Czy sklep nadal działa? */
        shm_lock(h.sem_id);
        int store_open = st->store_open;
        int accepting = st->cashier_accepting[cashier_id];
        int opened = st->cashier_open[cashier_id];
        int evacuated = st->evacuated;
        shm_unlock(h.sem_id);
        if (store_open != prev_store_open || opened != prev_opened ||
            accepting != prev_accepting || evacuated != prev_evacuated) {

            LOGF("kasjer",
                "Stan: store_open=%d opened=%d accepting=%d evacuated=%d (kasa=%d)",
                store_open, opened, accepting, evacuated, cashier_id);

            prev_store_open = store_open;
            prev_opened     = opened;
            prev_accepting  = accepting;
            prev_evacuated  = evacuated;
        }
        if (evacuated) break;

        /* Zamknięcie sklepu: dokończ kolejkę i wyjdź */
        if (!store_open) {
            LOGF("kasjer", "Sklep zamknięty – opróżniam kolejkę i kończę pracę.");
            while (1) {
                ClientMsg msg;
                ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, IPC_NOWAIT);
                if (r == -1) {
                    if (errno == ENOMSG) break;
                    if (errno == EINTR) continue;
                    perror("msgrcv (drain on store close)");
                    break;
                }
                
                if (g_evac) {
                    /* wiadomość zdjęta z kolejki MQ, więc licznik też zmniejszamy */
                    shm_lock(h.sem_id);
                    if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                    shm_unlock(h.sem_id);
                    break;
                }
                LOGF("kasjer", "Obsługuję klienta pid=%d (pozycji: %d)", (int)msg.client_pid, msg.item_count);
                for (int i = 0; i < msg.item_count; ++i) {
                    int pid = msg.items[i].product_id;
                    int qty = msg.items[i].quantity;
                    if (pid >= 0 && pid < st->P && qty > 0) {
                        LOGF("kasjer", "  - %s x%d", st->produkty[pid].nazwa, qty);
                    } else {
                        LOGF("kasjer", "  - (BŁĘDNY PRODUKT pid=%d, qty=%d)", pid, qty);
                    }
                }
                process_sale(st, h.sem_id, cashier_id, &msg);
                LOGF("kasjer", "Zakończyłem obsługę klienta pid=%d (kasa=%d)",
                    (int)msg.client_pid, cashier_id);
                shm_lock(h.sem_id);
                if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                shm_unlock(h.sem_id);
            }
            break;
        }

        /* Sklep otwarty */

        /* Kasa fizycznie zamknięta: czekaj na ponowne otwarcie */
        if (!opened) {
            msleep(200);
            continue;
        }

        if (!accepting) {
            /* Domknięcie kolejki po zamknięciu dla nowych */
            if (!said_not_accepting) {
                LOGF("kasjer", "Kasa %d nie przyjmuje nowych – domykam kolejkę.", cashier_id);
                said_not_accepting = 1;
            }
            int processed_any = 0;
            while (1) {
                ClientMsg msg;
                ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, IPC_NOWAIT);
                if (r == -1) {
                    if (errno == ENOMSG) break;
                    if (errno == EINTR) continue;
                    perror("msgrcv (drain on close-for-new)");
                    break;
                }
                processed_any = 1;
                if (g_evac) {
                    /* wiadomość zdjęta z kolejki MQ, więc licznik też zmniejszamy */
                    shm_lock(h.sem_id);
                    if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                    shm_unlock(h.sem_id);
                    break;
                }
                process_sale(st, h.sem_id, cashier_id, &msg);
                shm_lock(h.sem_id);
                if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                shm_unlock(h.sem_id);
            }

            /* Jeżeli kolejka pusta -> fizycznie zamknij kasę (proces dalej działa i może być ponownie otwarty) */
            shm_lock(h.sem_id);
            if (st->store_open && !st->cashier_accepting[cashier_id]) {
                if (st->cashier_queue_len[cashier_id] == 0) {
                    st->cashier_open[cashier_id] = 0;
                    LOGF("kasjer", "Kasa %d: kolejka pusta -> zamykam fizycznie.", cashier_id);
                }
            }
            shm_unlock(h.sem_id);

            if (!processed_any) msleep(100);
            continue;
        } else {
            said_not_accepting = 0;
        }

        ClientMsg msg;
        ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv");
            break;
        }

        if (g_evac) {
            /* wiadomość zdjęta z kolejki MQ, więc licznik też zmniejszamy */
            shm_lock(h.sem_id);
            if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
            shm_unlock(h.sem_id);
            break;
        }

        process_sale(st, h.sem_id, cashier_id, &msg);
        shm_lock(h.sem_id);
        if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
        shm_unlock(h.sem_id);
    }

    /* Inwentaryzacja: jeśli inventory_mode, wypisac podsumowanie */
    shm_lock(h.sem_id);
    int inv = st->inventory_mode;
    shm_unlock(h.sem_id);

    if (inv && !g_evac) {
        shm_lock(h.sem_id);
        print_summary(st, cashier_id);
        shm_unlock(h.sem_id);
    }

    if (g_evac) LOGF("kasjer", "Kończę pracę (ewakuacja).");
    else        LOGF("kasjer", "Kończę pracę.");

    ipc_detach_or_die(st);
    return 0;
}
