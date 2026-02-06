#include "common.h"

/*
 * cashier.c – proces kasjera:
 *  - odbiera wiadomości klientów z kolejki przypisanej do tej kasy
 *  - aktualizuje sold_by_cashier[cashier_id][Pi]
 *  - reaguje na zamykanie kasy: cashier_accepting=0 -> nie przyjmuje nowych, ale obsługuje kolejkę
 *  - przy inventory_mode wypisuje podsumowanie sprzedaży
 */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_evac = 0;
static void handler(int sig) {
    if (sig == SIG_EVAC) { g_evac = 1; g_stop = 1; g_common_stop = 1; }
    else if (sig == SIG_INV) {
        /* inwentaryzacja: nie zatrzymuje procesów */
        return;
    } else {
        g_stop = 1; /* SIGINT / SIGTERM */
        g_common_stop = 1;
    }
}

static void print_summary(const BakeryState* st, int cashier_id) {
    fprintf(stdout, "\n" COLOR_KASJER);
    fprintf(stdout, "╔══════════════════════════════════════════════════════════╗\n");
    fprintf(stdout, "║       🧾 INWENTARYZACJA - KASA %d - SPRZEDANE PRODUKTY    ║\n", cashier_id);
    fprintf(stdout, "╠══════════════════════════════════════════════════════════╣\n");
    fprintf(stdout, ANSI_RESET);
    
    int total_items = 0;
    double total_value = 0.0;
    
    for (int i = 0; i < st->P; ++i) {
        int qty = st->sold_by_cashier[cashier_id][i];
        if (qty > 0) {
            double value = qty * st->produkty[i].cena;
            fprintf(stdout, COLOR_KASJER "║" ANSI_RESET "  P%02d: %-25s %4d × %6.2f = " ANSI_BOLD "%8.2f zł" ANSI_RESET " " COLOR_KASJER "║" ANSI_RESET "\n", 
                    i, st->produkty[i].nazwa, qty, st->produkty[i].cena, value);
            total_items += qty;
            total_value += value;
        }
    }
    
    if (total_items == 0) {
        fprintf(stdout, COLOR_KASJER "║" ANSI_RESET "  (brak sprzedazy)                                        " COLOR_KASJER "║" ANSI_RESET "\n");
    }
    
    fprintf(stdout, COLOR_KASJER "╠══════════════════════════════════════════════════════════╣" ANSI_RESET "\n");
    fprintf(stdout, COLOR_KASJER "║" ANSI_RESET "  " ANSI_BOLD "SUMA KASA %d: %4d szt., wartość: %10.2f zł" ANSI_RESET "         " COLOR_KASJER "║" ANSI_RESET "\n", 
            cashier_id, total_items, total_value);
    fprintf(stdout, COLOR_KASJER "╚══════════════════════════════════════════════════════════╝" ANSI_RESET "\n");
}

/* Wysyła potwierdzenie do klienta że kasowanie zakończone */
static void send_reply(int msg_id, pid_t client_pid, int cashier_id, double total_price, int success) {
    CashierReply reply;
    reply.mtype = (long)client_pid;  /* klient odbiera po swoim PID */
    reply.cashier_id = cashier_id;
    reply.total_price = total_price;
    reply.success = success;
    
    if (msgsnd(msg_id, &reply, sizeof(CashierReply) - sizeof(long), IPC_NOWAIT) == -1) {
        if (errno != EIDRM && errno != EINVAL && errno != EAGAIN) {
            perror("msgsnd(reply to client)");
        }
    }
}

static double process_sale(BakeryState* st, int sem_id, int cashier_id, const ClientMsg* msg) {
    /* Księgowanie zakupów kasjera (sztuki per produkt) */
    LOGF("kasjer", "KASUJĘ: klient_pid=%d, pozycji=%d (kasa=%d)",
        (int)msg->client_pid, msg->item_count, cashier_id);
    
    double total_price = 0.0;
    
    shm_lock(sem_id);
    for (int i = 0; i < msg->item_count; ++i) {
        int pid = msg->items[i].product_id;
        int qty = msg->items[i].quantity;
        if (pid >= 0 && pid < st->P && qty > 0) {
            st->sold_by_cashier[cashier_id][pid] += qty;
            total_price += qty * st->produkty[pid].cena;
        }
    }
    shm_unlock(sem_id);

    /* Symulacja kasowania - czas proporcjonalny do liczby pozycji */
    msleep(rand_between(200, 400) + msg->item_count * 50);
    
    return total_price;
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
            while (!g_stop) {
                ClientMsg msg;
                ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, IPC_NOWAIT);
                if (r == -1) {
                    if (errno == ENOMSG) break;
                    if (errno == EINTR) continue;
                    if (errno == EIDRM || errno == EINVAL) break;  /* IPC zniszczone */
                    perror("msgrcv (drain on store close)");
                    break;
                }
                
                if (g_evac || g_stop) {
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
                double price1 = process_sale(st, h.sem_id, cashier_id, &msg);
                send_reply(h.msg_id[cashier_id], msg.client_pid, cashier_id, price1, 1);
                LOGF("kasjer", "Zakończyłem obsługę klienta pid=%d (kasa=%d, suma=%.2f zł)",
                    (int)msg.client_pid, cashier_id, price1);
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
            while (!g_stop) {
                ClientMsg msg;
                ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, IPC_NOWAIT);
                if (r == -1) {
                    if (errno == ENOMSG) break;
                    if (errno == EINTR) continue;
                    if (errno == EIDRM || errno == EINVAL) break;  /* IPC zniszczone */
                    perror("msgrcv (drain on close-for-new)");
                    break;
                }
                processed_any = 1;
                if (g_evac || g_stop) {
                    /* wiadomość zdjęta z kolejki MQ, więc licznik też zmniejszamy */
                    shm_lock(h.sem_id);
                    if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                    shm_unlock(h.sem_id);
                    /* Wyślij odpowiedź że przerwano (ewakuacja) */
                    send_reply(h.msg_id[cashier_id], msg.client_pid, cashier_id, 0.0, 0);
                    break;
                }
                double price2 = process_sale(st, h.sem_id, cashier_id, &msg);
                send_reply(h.msg_id[cashier_id], msg.client_pid, cashier_id, price2, 1);
                shm_lock(h.sem_id);
                if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
                shm_unlock(h.sem_id);
            }

            /* Jeżeli kolejka pusta -> po prostu czekaj na ponowne accepting=1 (manager) */
            shm_lock(h.sem_id);
            int q = st->cashier_queue_len[cashier_id];
            shm_unlock(h.sem_id);

            if (q == 0) {
                /* nic nie rób, nie zamykaj cashier_open — tylko manager steruje */
                msleep(100);
            }

            if (!processed_any) msleep(100);
            continue;
        } else {
            said_not_accepting = 0;
        }

        ClientMsg msg;
        ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            if (errno == EIDRM || errno == EINVAL) break;  /* IPC zniszczone */
            perror("msgrcv");
            break;
        }

        if (g_evac || g_stop) {
            /* wiadomość zdjęta z kolejki MQ, więc licznik też zmniejszamy */
            shm_lock(h.sem_id);
            if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
            shm_unlock(h.sem_id);
            /* Wyślij odpowiedź że przerwano (ewakuacja) */
            send_reply(h.msg_id[cashier_id], msg.client_pid, cashier_id, 0.0, 0);
            break;
        }

        double price3 = process_sale(st, h.sem_id, cashier_id, &msg);
        send_reply(h.msg_id[cashier_id], msg.client_pid, cashier_id, price3, 1);
        shm_lock(h.sem_id);
        if (st->cashier_queue_len[cashier_id] > 0) st->cashier_queue_len[cashier_id]--;
        shm_unlock(h.sem_id);
    }

    /* Inwentaryzacja: jeśli inventory_mode, wypisac podsumowanie */
    shm_lock(h.sem_id);
    int inv = st->inventory_mode;
    shm_unlock(h.sem_id);

    if (inv) {
        shm_lock(h.sem_id);
        print_summary(st, cashier_id);
        shm_unlock(h.sem_id);
    }

    if (g_evac) LOGF("kasjer", "Kończę pracę (ewakuacja).");
    else        LOGF("kasjer", "Kończę pracę.");

    ipc_detach_or_die(st);
    return 0;
}
