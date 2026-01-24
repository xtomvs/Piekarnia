#include "common.h"

/*
 * manager.c – program kierownika (główna pętla i sterowanie) i pętla sterująca symulacją.
 *
 * Zgodnie z Twoją decyzją:
 *  - to tutaj jest główna pętla programu, inicjalizacja IPC, uruchamianie procesów,
 *    polityka kas, generacja klientów, obsługa FIFO sterującego (opcjonalnie), sprzątanie.
 *  - pozostałe programy robią tylko własne funkcjonalności.
 */

/* Flagi ustawiane w handlerze sygnału */
static volatile sig_atomic_t g_sig_evac = 0;
static volatile sig_atomic_t g_sig_inv  = 0;
static volatile sig_atomic_t g_sig_term = 0;

static void signal_handler(int sig) {
    if (sig == SIG_EVAC) g_sig_evac = 1;
    else if (sig == SIG_INV) g_sig_inv = 1;
    else g_sig_term = 1; /* SIGINT / SIGTERM */
}

/* =========================
 *  Uruchamianie procesów
 * ========================= */

static pid_t spawn_process_or_die(const char* path, char* const argv[]) {
    pid_t pid = fork();
    if (pid == -1) DIE_PERROR("fork");

    if (pid == 0) {
        execv(path, argv);
        /* jeśli execv wrócił, to błąd */
        DIE_PERROR("execv");
    }
    return pid;
}

static void spawn_baker_or_die(void) {
    char* const argv[] = { "./baker", NULL };
    (void)spawn_process_or_die("./baker", argv);
}

static void spawn_cashiers_or_die(void) {
    for (int i = 0; i < CASHIERS; ++i) {
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%d", i);
        char* const argv[] = { "./cashier", idbuf, NULL };
        (void)spawn_process_or_die("./cashier", argv);
    }
}

static void spawn_client_or_die(void) {
    /* TODO: możesz przekazywać parametry klienta w argv (np. seed, id) */
    char* const argv[] = { "./client", NULL };
    (void)spawn_process_or_die("./client", argv);
}

/* =========================
 *  Polityka kas (szkic)
 * ========================= */

static int desired_open_cashiers(const BakeryState* st) {
    /* Zasad: K = N/3, min 1, max 3, zależnie od liczby klientów w sklepie. */
    int N = st->N;
    int K = (N > 0) ? (N / 3) : 1;
    if (K <= 0) K = 1;

    int c = st->customers_in_store;
    int want = (c + K - 1) / K; /* ceil(c/K) */
    if (want < 1) want = 1;
    if (want > CASHIERS) want = CASHIERS;

    /* Dodatkowa reguła z opisu: gdy < 2N/3, jedna kasa ma być zamknięta (zwykle wychodzi z want) */
    /* TODO: jeśli chcesz wymusić konkretnie: gdy c < 2N/3 => want <= 2 */
    if (c < (2 * N) / 3 && want == 3) want = 2;

    return want;
}

static void apply_cashier_policy(BakeryState* st, int sem_id) {
    shm_lock_or_die(sem_id);

    int want = desired_open_cashiers(st);

    /* policzyc aktualnie otwarte */
    int open = 0;
    for (int i = 0; i < CASHIERS; ++i) if (st->cashier_open[i]) open++;

    /* TODO: strategia wyboru, które otwierać / zamykać.
     * Propozycja:
     *  - otwieraj od 0 w górę
     *  - zamykac od końca (2->1->0) i ustawic cashier_accepting=0, ale cashier obsłuży swoich w kolejce.
     */

    /* Otwieranie */
    for (int i = 0; i < CASHIERS && open < want; ++i) {
        if (!st->cashier_open[i]) {
            st->cashier_open[i] = 1;
            st->cashier_accepting[i] = 1;
            open++;
            /* TODO: log */
        }
    }

    /* Zamykanie (przestaje przyjmować nowych) */
    for (int i = CASHIERS - 1; i >= 0 && open > want; --i) {
        if (st->cashier_open[i]) {
            /* nigdy nie zamykac, jeśli to jedyna otwarta */
            if (open <= 1) break;

            st->cashier_accepting[i] = 0; /* kluczowe: stara kolejka ma być obsłużona */
            /* TODO: kasjer po opróżnieniu kolejki ustawi cashier_open[i]=0 (lub zrobi to kierownik) */
            open--;
            /* TODO: log */
        }
    }

    shm_unlock_or_die(sem_id);
}

/* 
 FIFO sterujące (opcjonalnie)
*/

static int ctrl_fifo_open_or_off(void) {
    /* TODO: jeśli nie FIFO – usunac całkowicie i zostawic tylko sygnały */
    if (mkfifo(CTRL_FIFO_PATH, FIFO_PERMS_MIN) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo(CTRL_FIFO_PATH)");
            return -1;
        }
    }
    int fd = open(CTRL_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open(CTRL_FIFO_PATH)");
        return -1;
    }
    return fd;
}

static void ctrl_fifo_poll(int fd) {
    if (fd < 0) return;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Proste komendy: EVAC, INV, STATUS */
    if (strstr(buf, "EVAC")) {
        g_sig_evac = 1;
    } else if (strstr(buf, "INV")) {
        g_sig_inv = 1;
    } else if (strstr(buf, "STATUS")) {
        /* TODO: można ustawić flagę i wypisać status w pętli */
    }
}

/* =========================
 *  Main
 * ========================= */

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    install_signal_handlers_or_die(signal_handler);

    /* ====== TODO: parsowanie konfiguracji z argv lub stdin ======
     * Minimalnie: wpisac na sztywno poprawne wartości, żeby ruszyć.
     * Docelowo: P>10, N, Tp/Tk, ceny, Ki.
     */
    int P = 12;
    int N = 30;
    int Tp = 0;     /* TODO: zrobic realny czas */
    int Tk = 60;    /* TODO: zrobic realny czas / długość symulacji */
    int prices[MAX_P];
    int Ki[MAX_P];

    for (int i = 0; i < P; ++i) {
        prices[i] = 5 + i;          /* TODO: wczytaj z wejścia */
        Ki[i] = 10 + (i % 5);       /* TODO: wczytaj z wejścia */
    }

    if (!validate_config(P, N, Tp, Tk, Ki, prices)) {
        fprintf(stderr, "Błędna konfiguracja. Sprawdź P>10, N>0, Tp<Tk, Ki/prices.\n");
        return EXIT_FAILURE;
    }

    /* ====== IPC init ====== */
    IpcHandles h;
    memset(&h, 0, sizeof(h));
    h.shm_id = h.sem_id = -1;
    for (int i = 0; i < CASHIERS; ++i) h.msg_id[i] = -1;

    ipc_create_or_die(&h, P);

    BakeryState* st = NULL;
    ipc_attach_or_die(&h, &st);

    /* Ustawic konfigurację w SHM */
    shm_lock_or_die(h.sem_id);
    st->P = P;
    st->N = N;
    st->open_hour = Tp;
    st->close_hour = Tk;

    for (int i = 0; i < P; ++i) {
        st->prices[i] = prices[i];
        st->Ki[i] = Ki[i];

        st->conveyors[i].capacity = Ki[i];
        st->conveyors[i].head = 0;
        st->conveyors[i].tail = 0;
        st->conveyors[i].count = 0;
        /* items[] zostaje 0 */
    }
    shm_unlock_or_die(h.sem_id);

    /* Ustawic semafory: store slots = N, empty[i]=Ki[i] */
    {
        int sem_n = sem_count_for_P(P);
        /* Pobrac aktualne i zmienic wybrane (prosto: semctl SETVAL) */
        union semun arg;

        arg.val = N;
        CHECK_SYS(semctl(h.sem_id, SEM_STORE_SLOTS, SETVAL, arg), "semctl(SETVAL STORE_SLOTS)");

        for (int i = 0; i < P; ++i) {
            arg.val = Ki[i];
            CHECK_SYS(semctl(h.sem_id, SEM_CONV_EMPTY(i), SETVAL, arg), "semctl(SETVAL EMPTY)");
            /* MUTEX i FULL już OK */
        }

        (void)sem_n; /* TODO: usunac jeśli nieużywane */
    }

    /* ====== Uruchom procesy ====== */
    spawn_baker_or_die();
    spawn_cashiers_or_die();

    /* Opcjonalny FIFO */
    int fifo_fd = ctrl_fifo_open_or_off();

    /* ====== Główna pętla symulacji ====== */
    /* TODO: zastapic Tk na realny zegar (Tp..Tk), np. time(NULL) i progi czasu */
    int sim_time = 0;

    while (!g_sig_term) {
        /* Obsługa FIFO */
        ctrl_fifo_poll(fifo_fd);

        /* Obsługa sygnałów */
        if (g_sig_evac) {
            shm_lock_or_die(h.sem_id);
            st->evacuated = 1;
            st->store_open = 0;
            shm_unlock_or_die(h.sem_id);

            /* Wyślij ewakuację do grupy procesów */
            /* TODO: ustawic grupę procesów (setpgid) i zrobic kill(-pgid, SIG_EVAC) */
            /* Na razie: wyślij do siebie (potem do wszystkich) */
            kill(0, SIG_EVAC);

            break;
        }

        if (g_sig_inv) {
            shm_lock_or_die(h.sem_id);
            st->inventory_mode = 1;
            shm_unlock_or_die(h.sem_id);
            g_sig_inv = 0;
        }

        /* Polityka kas */
        apply_cashier_policy(st, h.sem_id);

        /* Generacja klientów */
        /* TODO: dodac limit MAX_CLIENTS_TOTAL i walidację wejścia */
        int should_spawn = (rand_between(0, 100) < 35); /* ~35% iteracji */
        if (should_spawn) {
            /* TODO: nie spawnuj po zamknięciu sklepu */
            spawn_client_or_die();
        }

        /* Koniec dnia (Tk) */
        sim_time += 1;
        if (sim_time >= Tk) {
            shm_lock_or_die(h.sem_id);
            st->store_open = 0; /* sygnał dla klientów: nie wchodzić nowym */
            shm_unlock_or_die(h.sem_id);
            break;
        }

        msleep(200);
    }

    /* ====== Faza zamykania ====== */

    /* TODO: poczekac aż klienci wyjdą (customers_in_store==0) */
    /* TODO: zamknac kasy poprawnie (cashier_accepting=0) */
    /* TODO: inwentaryzacja: raporty kasjerów, sumy kierownika i piekarza */

    /* Poczekaj na dzieci */
    /* TODO: przechwycic konkretne PID-y i robić waitpid, tu tylko "sprzątanie ogólne" */
    int status;
    while (wait(&status) > 0) {
        /* TODO: log exit codes */
    }

    if (fifo_fd >= 0) close(fifo_fd);
    /* TODO: unlink CTRL_FIFO_PATH jeśli używany */
    /* unlink(CTRL_FIFO_PATH); */

    ipc_detach_or_die(st);
    ipc_destroy_or_die(&h, P);

    return 0;
}
