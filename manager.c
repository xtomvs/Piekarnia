#include "common.h"

/*
 * manager.c – program kierownika (główna pętla i sterowanie) i pętla sterująca symulacją.
 *
 * Zgodnie z Twoją decyzją:
 *  - to tutaj jest główna pętla programu, inicjalizacja IPC, uruchamianie procesów,
 *    polityka kas, generacja klientów, obsługa FIFO sterującego (opcjonalnie), sprzątanie.
 *  - pozostałe programy robią tylko własne funkcjonalności.
 */

#define MAX_CLIENTS_TOTAL 50     
#define SPAWN_COOLDOWN_MS 150 /* max 1 klient na 300ms */

/* Flagi ustawiane w handlerze sygnału */
static volatile sig_atomic_t g_sig_evac = 0;
static volatile sig_atomic_t g_sig_inv  = 0;
static volatile sig_atomic_t g_sig_term = 0;

static pid_t g_pgid = -1;

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
    char* const argv[] = { "./client", NULL };
    (void)spawn_process_or_die("./client", argv);
}

/* =========================
 *  Polityka kas (szkic)
 * ========================= */

static int desired_open_cashiers(const BakeryState* st) {
    /* Zasad: K = N/3, min 1, max 3, zależnie od liczby klientów w sklepie. */
    static int last = 1;          /* pamięta poprzednią decyzję */
    int c = st->customers_in_store;
    int N = st->N;

    int t1_on  = (N / 3) + 1;         /* np. 4 */
    int t1_off = (N / 3) - 1;         /* np. 2 */
    int t2_on  = (2 * N / 3) + 1;     /* np. 7 */
    int t2_off = (2 * N / 3) - 1;     /* np. 5 */

    if (t1_off < 0) t1_off = 0;
    if (t2_off < 0) t2_off = 0;

    if (last == 1) {
        if (c >= t2_on) last = 3;
        else if (c >= t1_on) last = 2;
    } else if (last == 2) {
        if (c >= t2_on) last = 3;
        else if (c <= t1_off) last = 1;
    } else { /* last == 3 */
        if (c <= t2_off) last = 2;
    }

    return last;
}

static void apply_cashier_policy(BakeryState* st, int sem_id) {
    shm_lock(sem_id);

    int want = desired_open_cashiers(st);

    /* procesy kasjerów istnieją cały czas -> open=1 */
    for (int i = 0; i < CASHIERS; ++i) st->cashier_open[i] = 1;

    int a0 = 1;
    int a1 = (want >= 2);
    int a2 = (want >= 3);

    if (st->cashier_accepting[0] != a0) {
        st->cashier_accepting[0] = a0;
        LOGF("kierownik", "Kasa 0 accepting=%d", a0);
    }
    if (st->cashier_accepting[1] != a1) {
        st->cashier_accepting[1] = a1;
        LOGF("kierownik", "Kasa 1 accepting=%d", a1);
    }
    if (st->cashier_accepting[2] != a2) {
        st->cashier_accepting[2] = a2;
        LOGF("kierownik", "Kasa 2 accepting=%d", a2);
    }

    shm_unlock(sem_id);
}

/* 
 FIFO sterujące 
*/

static int ctrl_fifo_open_or_off(void) {
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

static int current_hour_local(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour;
}

static long long now_ms(void) {
    struct timespec ts;
    CHECK_SYS(clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void reap_children_nonblocking(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            LOGF("kierownik", "Proces potomny pid=%d zakończył się kodem=%d",
                 (int)pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOGF("kierownik", "Proces potomny pid=%d zakończony sygnałem=%d",
                 (int)pid, WTERMSIG(status));
        } else {
            LOGF("kierownik", "Proces potomny pid=%d zakończony (status=%d)",
                 (int)pid, status);
        }
    }

    /* waitpid == 0 -> brak zakończonych dzieci, waitpid == -1 -> np. brak dzieci (ECHILD) */
}


/* =========================
 *  Main
 * ========================= */

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    (void)argc; (void)argv;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    install_signal_handlers_or_die(signal_handler);

    /* Utwórz osobną grupę procesów dla symulacji (żeby kill(-pgid, ...) nie dotknął powłoki) */
    CHECK_SYS(setpgid(0, 0), "setpgid(manager)");
    g_pgid = getpgrp();


    int P = 15;
    int N = 9;
    int Tp = 6;     
    int Tk = 20;    
    Product produkty[MAX_P];
    int Ki[MAX_P];
    int spawned_clients_total = 0;
    long long last_spawn_ms = 0;
    long long last_policy_ms = 0;

    /* Domyślna lista produktów (P=15) */
    memset(produkty, 0, sizeof(produkty));
    strcpy(produkty[0].nazwa, "Bułka kajzerka");             produkty[0].cena = 3.0;
    strcpy(produkty[1].nazwa, "Bułka grahamka");            produkty[1].cena = 4.0;
    strcpy(produkty[2].nazwa, "Chleb pszenny");             produkty[2].cena = 6.0;
    strcpy(produkty[3].nazwa, "Chleb pełnoziarnisty");      produkty[3].cena = 7.0;
    strcpy(produkty[4].nazwa, "Chleb żytni");               produkty[4].cena = 8.0;
    strcpy(produkty[5].nazwa, "Bagietka");                  produkty[5].cena = 9.0;
    strcpy(produkty[6].nazwa, "Chleb na zakwasie");         produkty[6].cena = 10.0;
    strcpy(produkty[7].nazwa, "Pieczywo bezglutenowe");     produkty[7].cena = 11.0;
    strcpy(produkty[8].nazwa, "Pączek");                    produkty[8].cena = 2.0;
    strcpy(produkty[9].nazwa, "Rogalik");                   produkty[9].cena = 12.0;
    strcpy(produkty[10].nazwa, "Ciastko kruche");           produkty[10].cena = 1.0;
    strcpy(produkty[11].nazwa, "Strucla");                  produkty[11].cena = 13.0;
    strcpy(produkty[12].nazwa, "Zapiekanka");               produkty[12].cena = 14.0;
    strcpy(produkty[13].nazwa, "Focaccia");                 produkty[13].cena = 15.0;
    strcpy(produkty[14].nazwa, "Rogal świętomarciński");    produkty[14].cena = 16.0;

    for (int i = 0; i < P; ++i) {
        Ki[i] = 10 + (i % 5);     
    }

    if (!validate_config(P, N, Tp, Tk, Ki, produkty)) {
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
    shm_lock(h.sem_id);
    st->P = P;
    st->N = N;
    st->open_hour = Tp;
    st->close_hour = Tk;

    st->store_open = 1;
    st->evacuated = 0;
    st->inventory_mode = 0;
    st->customers_in_store = 0;

    for (int i = 0; i < P; ++i) {
        st->produkty[i] = produkty[i];
        st->Ki[i] = Ki[i];
        st->produced[i] = 0;
        st->wasted[i] = 0;

        st->conveyors[i].capacity = Ki[i];
        st->conveyors[i].head = 0;
        st->conveyors[i].tail = 0;
        st->conveyors[i].count = 0;
        /* items[] zostaje 0 */
    }

    for (int c = 0; c < CASHIERS; ++c) {
        st->cashier_open[c] = 1;       /* albo 1 tylko dla kasy 0, jeśli chcesz min 1 na start */
        st->cashier_accepting[c] = 1;  /* jw. */
        st->cashier_queue_len[c] = 0;
        for (int i = 0; i < P; ++i) st->sold_by_cashier[c][i] = 0;
    }
    shm_unlock(h.sem_id);
    LOGF("kierownik", "Start symulacji: P=%d, N=%d, godziny %d-%d", P, N, Tp, Tk);
    LOGF("kierownik", "IPC: shm_id=%d, sem_id=%d, msg=[%d,%d,%d]",
        h.shm_id, h.sem_id, h.msg_id[0], h.msg_id[1], h.msg_id[2]);
    

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
    LOGF("kierownik", "Uruchomiono piekarza i %d kasjerów", CASHIERS);

    /* Opcjonalny FIFO */
    int fifo_fd = ctrl_fifo_open_or_off();

    /* ====== Główna pętla symulacji ====== */
    

    while (!g_sig_term) {
        /* Obsługa FIFO */
        ctrl_fifo_poll(fifo_fd);

        reap_children_nonblocking();

        /* Obsługa sygnałów */
        if (g_sig_evac) {
            shm_lock(h.sem_id);
            st->evacuated = 1;
            st->store_open = 0;
            shm_unlock(h.sem_id);

            /* Wyślij ewakuację do grupy procesów */
            if (g_pgid > 0) {
                LOGF("kierownik", "EWAKUACJA! Wysyłam sygnał do wszystkich procesów.");
                CHECK_SYS(kill(-g_pgid, SIG_EVAC), "kill(-pgid, SIG_EVAC)");
            } else {
                LOGF("kierownik", "EWAKUACJA! Wysyłam sygnał do wszystkich procesów.");
                CHECK_SYS(kill(0, SIG_EVAC), "kill(0, SIG_EVAC)");
            }

            break;
        }

        if (g_sig_inv) {
            shm_lock(h.sem_id);
            st->inventory_mode = 1;
            shm_unlock(h.sem_id);
            LOGF("kierownik", "INWENTARYZACJA: tryb włączony (klienci kupują do zamknięcia).");
            g_sig_inv = 0;
        }

        int hour = current_hour_local();

        if (hour < Tp) {
        msleep(500);
        continue;
        }

        if (hour >= Tk) {
        shm_lock(h.sem_id);
        st->store_open = 0;
        shm_unlock(h.sem_id);
        LOGF("kierownik", "Zamknięcie sklepu (godzina=%d >= %d).", hour, Tk);
        break;
        }

        /* Polityka kas */
        long long tnow = now_ms();
        if (tnow - last_policy_ms >= 500) {
            apply_cashier_policy(st, h.sem_id);
            last_policy_ms = tnow;
        }

        /* Generacja klientów */
        int should_spawn = (rand_between(0, 100) < 35); /* ~35% iteracji */
        if (should_spawn) {
            long long t = now_ms();

            /* rate limit */
            if (t - last_spawn_ms < SPAWN_COOLDOWN_MS) {
                /* za szybko – pomijamy */
            } else if (spawned_clients_total >= MAX_CLIENTS_TOTAL) {
                /* osiągnięto limit – pomijamy */
            } else {
                /* Nie spawnuj po zamknięciu sklepu (lub po ewakuacji) */
                shm_lock(h.sem_id);
                int open_now = (st->store_open && !st->evacuated);
                shm_unlock(h.sem_id);

                if (open_now) {
                    spawn_client_or_die();
                    LOGF("kierownik", "Nowy klient (łącznie: %d/%d)", spawned_clients_total, MAX_CLIENTS_TOTAL);
                    spawned_clients_total++;
                    last_spawn_ms = t;
                }
            }
        }

        msleep(200); /* główna pętla co 200ms */
    }
    
    
        

    /* ====== Faza zamykania ====== */
    //TODO: inwentaryzacja
    /* 1) Zamknij kasy dla nowych klientów (kasjerzy domkną kolejki) */
    LOGF("kierownik", "Zamykanie kas dla nowych klientów (domykanie kolejek).");
    shm_lock(h.sem_id);
    for (int i = 0; i < CASHIERS; ++i) {
        st->cashier_accepting[i] = 0;
        /* st->cashier_open[i] zostawiamy bez zmian:
           kasjer sam domknie kolejkę i (w Twojej wersji cashier.c) ustawi cashier_open=0 */
    }
    shm_unlock(h.sem_id);

    while (1) {
        shm_lock(h.sem_id);
        int in_store = st->customers_in_store;
        shm_unlock(h.sem_id);

        if (in_store <= 0) break;
        msleep(200);
    }

    LOGF("kierownik", "Wszyscy klienci opuścili sklep.");

    /* 3) Poczekaj na dzieci */
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

        
