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
    if (sig == SIG_EVAC) g_evac = 1;
    g_stop = 1;
}

static void print_summary(const BakeryState* st, int cashier_id) {
    /* TODO:  raport */
    fprintf(stdout, "\n=== PODSUMOWANIE KASY %d ===\n", cashier_id);
    for (int i = 0; i < st->P; ++i) {
        int s = st->sold_by_cashier[cashier_id][i];
        if (s > 0) fprintf(stdout, "Produkt %d: %d szt.\n", i, s);
    }
}

int main(int argc, char** argv) {
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

    
    h.sem_id = semget(bakery_ftok_or_die(0x42), sem_count_for_P(MAX_P), IPC_PERMS_MIN);
    if (h.sem_id == -1) DIE_PERROR("semget(cashier)");

    for (int i = 0; i < CASHIERS; ++i) {
        h.msg_id[i] = msgget(bakery_ftok_or_die(0x50 + i), IPC_PERMS_MIN);
        if (h.msg_id[i] == -1) DIE_PERROR("msgget(cashier)");
    }

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    while (!g_stop) {
        /* Czy sklep nadal działa? */
        shm_lock_or_die(h.sem_id);
        int store_open = st->store_open;
        int inventory_mode = st->inventory_mode;
        int accepting = st->cashier_accepting[cashier_id];
        int opened = st->cashier_open[cashier_id];
        int evacuated = st->evacuated;
        shm_unlock_or_die(h.sem_id);

        if (evacuated) break;

        /* Jeśli zamknięta – możesz spać i czekać na ponowne otwarcie lub na koniec dnia */
        if (!opened) {
            msleep(200);
            if (!store_open) break;
            continue;
        }

        /* Odbiór wiadomości: jeśli accepting=0, nadal obsłuzyc starą kolejkę, ale nie przyjmowac nowych?
         *
         * TODO: dopracowac, by uniknąć wyścigu w momencie zamykania.
         */
        ClientMsg msg;
        ssize_t r = msgrcv(h.msg_id[cashier_id], &msg, sizeof(ClientMsg) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv");
            break;
        }

        /* Jeśli ewakuacja przyszła "w trakcie", nie liczyc transakcji */
        if (g_evac) break;

        /* Księgowanie sprzedaży */
        shm_lock_or_die(h.sem_id);
        for (int i = 0; i < msg.item_count; ++i) {
            int pid = msg.items[i].product_id;
            int qty = msg.items[i].quantity;
            if (pid >= 0 && pid < st->P && qty > 0) {
                st->sold_by_cashier[cashier_id][pid] += qty;
            }
            /* TODO: walidacja komunikatu (bezpiecznie) */
        }
        shm_unlock_or_die(h.sem_id);

        /* TODO: "wydruk paragonu" */
        (void)accepting;

        /* Jeśli sklep zamknięty i kolejka pusta, wyjdź – ale nie wiemy czy pusta bez IPC_NOWAIT.
         * TODO: po store_open=0 przełączyc na msgrcv IPC_NOWAIT, opróżnij do ENOMSG i zakończ.
         */
        if (!store_open && inventory_mode) {
            /* placeholder – prawdziwa logika w TODO */
        }
    }

    /* Inwentaryzacja: jeśli inventory_mode, wypisac podsumowanie */
    shm_lock_or_die(h.sem_id);
    int inv = st->inventory_mode;
    shm_unlock_or_die(h.sem_id);

    if (inv && !g_evac) {
        shm_lock_or_die(h.sem_id);
        print_summary(st, cashier_id);
        shm_unlock_or_die(h.sem_id);
    }

    ipc_detach_or_die(st);
    return 0;
}
