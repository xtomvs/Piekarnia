#include "common.h"

/*
 * manager.c – program kierownika (glowna petla i sterowanie) i petla sterujaca symulacja.
 *
 * TRYBY PRACY:
 *   ./manager           - normalny tryb pracy (sklep otwarty wg godzin)
 *   ./manager test N    - test przeciazeniowy z N klientami (domyslnie 1000)
 *   ./manager stress    - test stresu z maksymalna liczba klientow
 */

#define MAX_CLIENTS_TOTAL 1000  
#define SPAWN_COOLDOWN_MS 0     /* minimalny odstep miedzy spawnem klientow (ms) */

/* Flagi trybu testowego */
static int g_test_mode = 0;
static int g_stress_mode = 0;
static int g_test_client_count = 1000; 

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
 *  Polityka kas 
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
        /* Można rozszerzyć o wypisanie statusu */
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
 *  Statystyki testow
 * ========================= */

typedef struct TestStats {
    int clients_spawned;
    int clients_entered;
    int clients_completed;
    int max_concurrent;
    int waiting_clients;
    long long start_time_ms;
    long long end_time_ms;
} TestStats;

static TestStats g_stats = {0};

static void print_test_stats(const BakeryState* st) {
    printf("\n========== STATYSTYKI TESTU ==========\n");
    printf("Klientow wygenerowanych: %d\n", g_stats.clients_spawned);
    printf("Max rownoczesnie w sklepie: %d (limit N=%d)\n", g_stats.max_concurrent, st->N);
    printf("Czas trwania testu: %lld ms\n", g_stats.end_time_ms - g_stats.start_time_ms);
    
    int total_sold = 0;
    int total_wasted = 0;
    int total_produced = 0;
    for (int i = 0; i < st->P; ++i) {
        total_produced += st->produced[i];
        total_wasted += st->wasted[i];
        for (int c = 0; c < CASHIERS; ++c) {
            total_sold += st->sold_by_cashier[c][i];
        }
    }
    
    printf("Produktow wyprodukowanych: %d\n", total_produced);
    printf("Produktow sprzedanych: %d\n", total_sold);
    printf("Produktow zmarnowanych (ewakuacja): %d\n", total_wasted);
    printf("========================================\n\n");
}


/* =========================
 *  Main
 * ========================= */

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    
    /* Parsowanie argumentow */
    if (argc >= 2) {
        if (strcmp(argv[1], "test") == 0) {
            g_test_mode = 1;
            if (argc >= 3) {
                g_test_client_count = atoi(argv[2]);
                if (g_test_client_count <= 0) g_test_client_count = 1000;
            }
            printf("=== TRYB TESTOWY: %d klientow ===\n", g_test_client_count);
        } else if (strcmp(argv[1], "stress") == 0) {
            g_stress_mode = 1;
            g_test_mode = 1;
            g_test_client_count = 5000;
            printf("=== TRYB STRESS: %d klientow ===\n", g_test_client_count);
        }
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    install_signal_handlers_or_die(signal_handler);

    /* Utworz osobna grupe procesow dla symulacji (zeby kill(-pgid, ...) nie dotknol powloki) */
    CHECK_SYS(setpgid(0, 0), "setpgid(manager)");
    g_pgid = getpgrp();


    int P = 15;
    int N = 30;        /* limit klientow w sklepie */
    int Tp = 0;        /* otwarcie: 0:00 (zawsze otwarty) */
    int Tk = 24;       /* zamkniecie: 24:00 (nigdy nie zamknie automatycznie) */
    Product produkty[MAX_P];
    int Ki[MAX_P];
    int spawned_clients_total = 0;
    long long last_spawn_ms = 0;
    long long last_policy_ms = 0;
    long long last_stats_ms = 0;

    /* Domyslna lista produktow (P=15) */
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
        union semun arg;

        arg.val = N;
        CHECK_SYS(semctl(h.sem_id, SEM_STORE_SLOTS, SETVAL, arg), "semctl(SETVAL STORE_SLOTS)");

        for (int i = 0; i < P; ++i) {
            arg.val = Ki[i];
            CHECK_SYS(semctl(h.sem_id, SEM_CONV_EMPTY(i), SETVAL, arg), "semctl(SETVAL EMPTY)");
        }
    }

    /* ====== Uruchom procesy ====== */
    spawn_baker_or_die();
    spawn_cashiers_or_die();
    LOGF("kierownik", "Uruchomiono piekarza i %d kasjerow", CASHIERS);

    /* Opcjonalny FIFO */
    int fifo_fd = ctrl_fifo_open_or_off();

    /* Inicjalizacja statystyk */
    g_stats.start_time_ms = now_ms();
    
    int max_clients = g_test_mode ? g_test_client_count : MAX_CLIENTS_TOTAL;

    /* ====== Glowna petla symulacji ====== */
    
    while (!g_sig_term) {
        /* Obsluga FIFO */
        ctrl_fifo_poll(fifo_fd);

        /* Zbieraj dzieci (zombie) */
        reap_children_nonblocking();

        /* Obsluga sygnalow */
        if (g_sig_evac) {
            shm_lock(h.sem_id);
            st->evacuated = 1;
            st->store_open = 0;
            shm_unlock(h.sem_id);

            /* Wyslij ewakuacje do grupy procesow */
            LOGF("kierownik", "EWAKUACJA! Wysylam sygnal do wszystkich procesow.");
            if (g_pgid > 0) {
                CHECK_SYS(kill(-g_pgid, SIG_EVAC), "kill(-pgid, SIG_EVAC)");
            } else {
                CHECK_SYS(kill(0, SIG_EVAC), "kill(0, SIG_EVAC)");
            }

            break;
        }

        if (g_sig_inv) {
            shm_lock(h.sem_id);
            st->inventory_mode = 1;
            shm_unlock(h.sem_id);
            LOGF("kierownik", "INWENTARYZACJA: tryb wlaczony (klienci kupuja do zamkniecia).");
            g_sig_inv = 0;
        }

        /* W trybie testowym ignorujemy godziny */
        if (!g_test_mode) {
            int hour = current_hour_local();

            if (hour < Tp) {
                msleep(500);
                continue;
            }

            if (hour >= Tk) {
                shm_lock(h.sem_id);
                st->store_open = 0;
                shm_unlock(h.sem_id);
                LOGF("kierownik", "Zamkniecie sklepu (godzina=%d >= %d).", hour, Tk);
                break;
            }
        }

        /* Polityka kas */
        long long tnow = now_ms();
        if (tnow - last_policy_ms >= 500) {
            apply_cashier_policy(st, h.sem_id);
            last_policy_ms = tnow;
        }

        /* Aktualizuj statystyki */
        if (tnow - last_stats_ms >= 1000) {
            shm_lock(h.sem_id);
            int curr = st->customers_in_store;
            if (curr > g_stats.max_concurrent) {
                g_stats.max_concurrent = curr;
            }
            shm_unlock(h.sem_id);
            
            if (g_test_mode && (spawned_clients_total % 100 == 0 || spawned_clients_total == max_clients)) {
                LOGF("kierownik", "[STATS] Spawned=%d/%d, InStore=%d, MaxConcurrent=%d",
                     spawned_clients_total, max_clients, curr, g_stats.max_concurrent);
            }
            last_stats_ms = tnow;
        }

        /* Generacja klientow */
        int should_spawn = g_test_mode ? 1 : (rand_between(0, 100) < 35);
        if (should_spawn) {
            long long t = now_ms();

            /* rate limit */
            if (t - last_spawn_ms < SPAWN_COOLDOWN_MS) {
                /* za szybko - pomijamy */
            } else if (spawned_clients_total >= max_clients) {
                /* osiagnieto limit - zakonczmy test */
                if (g_test_mode) {
                    LOGF("kierownik", "Wygenerowano wszystkich %d klientow. Czekam na zakonczenie...", max_clients);
                    break;
                }
            } else {
                /* Nie spawnuj po zamknieciu sklepu (lub po ewakuacji) */
                shm_lock(h.sem_id);
                int open_now = (st->store_open && !st->evacuated);
                shm_unlock(h.sem_id);

                if (open_now) {
                    spawn_client_or_die();
                    spawned_clients_total++;
                    g_stats.clients_spawned = spawned_clients_total;
                    last_spawn_ms = t;
                    
                    /* W trybie stress spawnuj szybciej */
                    if (!g_stress_mode && spawned_clients_total % 50 == 0) {
                        LOGF("kierownik", "Nowy klient (lacznie: %d/%d)", spawned_clients_total, max_clients);
                    }
                }
            }
        }

        msleep(g_stress_mode ? 1 : 10); /* glowna petla */
    }
    
    /* ====== Faza zamykania ====== */
    
    /* W trybie testowym poczekaj az klienci zrobia zakupy */
    if (g_test_mode) {
        LOGF("kierownik", "Czekam az klienci zrobia zakupy (sklep nadal otwarty)...");
        int wait_shopping = 0;
        while (wait_shopping < 50) { /* max 5 sekund */
            shm_lock(h.sem_id);
            int in_store = st->customers_in_store;
            shm_unlock(h.sem_id);
            if (in_store == 0) break;
            msleep(100);
            wait_shopping++;
        }
    }
    
    /* Zamknij sklep */
    shm_lock(h.sem_id);
    st->store_open = 0;
    shm_unlock(h.sem_id);
    
    LOGF("kierownik", "Zamykanie kas dla nowych klientow (domykanie kolejek).");
    shm_lock(h.sem_id);
    for (int i = 0; i < CASHIERS; ++i) {
        st->cashier_accepting[i] = 0;
    }
    shm_unlock(h.sem_id);

    /* Czekaj az wszyscy klienci wyjda */
    int wait_counter = 0;
    int max_wait = g_test_mode ? 600 : 300; /* max 60s lub 30s */
    while (wait_counter < max_wait) {
        shm_lock(h.sem_id);
        int in_store = st->customers_in_store;
        shm_unlock(h.sem_id);

        if (in_store <= 0) break;
        
        if (wait_counter % 50 == 0) {
            LOGF("kierownik", "Czekam na wyjscie klientow: %d pozostalo w sklepie", in_store);
        }
        
        msleep(100);
        wait_counter++;
    }
    
    if (wait_counter >= max_wait) {
        LOGF("kierownik", "TIMEOUT: Wymuszam zamkniecie (klienci mogli sie zablokowac)");
    }

    g_stats.end_time_ms = now_ms();
    LOGF("kierownik", "Wszyscy klienci opuscili sklep.");

    /* Inwentaryzacja kierownika: towar na podajnikach */
    shm_lock(h.sem_id);
    int inv_mode = st->inventory_mode;
    shm_unlock(h.sem_id);
    
    if (inv_mode) {
        fprintf(stdout, "\n" COLOR_KIEROWNIK);
        fprintf(stdout, "╔══════════════════════════════════════════════════════════╗\n");
        fprintf(stdout, "║   📦 INWENTARYZACJA - KIEROWNIK - TOWAR NA PODAJNIKACH   ║\n");
        fprintf(stdout, "╠══════════════════════════════════════════════════════════╣\n");
        fprintf(stdout, ANSI_RESET);
        
        int total_on_conveyors = 0;
        shm_lock(h.sem_id);
        for (int i = 0; i < st->P; ++i) {
            int on_conv = st->conveyors[i].count;
            if (on_conv > 0) {
                fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  P%02d: %-30s %6d szt.        " COLOR_KIEROWNIK "║" ANSI_RESET "\n", 
                        i, st->produkty[i].nazwa, on_conv);
                total_on_conveyors += on_conv;
            }
        }
        shm_unlock(h.sem_id);
        
        if (total_on_conveyors == 0) {
            fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  (wszystkie podajniki puste)                             " COLOR_KIEROWNIK "║" ANSI_RESET "\n");
        }
        
        fprintf(stdout, COLOR_KIEROWNIK);
        fprintf(stdout, "╠══════════════════════════════════════════════════════════╣\n");
        fprintf(stdout, ANSI_RESET);
        fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  " ANSI_BOLD "SUMA NA PODAJNIKACH: %6d szt." ANSI_RESET "                         " COLOR_KIEROWNIK "║" ANSI_RESET "\n", total_on_conveyors);
        fprintf(stdout, COLOR_KIEROWNIK "╚══════════════════════════════════════════════════════════╝" ANSI_RESET "\n");
        
        /* Podsumowanie calkowite sprzedazy ze wszystkich kas */
        fprintf(stdout, "\n" COLOR_KIEROWNIK);
        fprintf(stdout, "╔══════════════════════════════════════════════════════════╗\n");
        fprintf(stdout, "║   💰 INWENTARYZACJA - PODSUMOWANIE CAŁKOWITE SPRZEDAŻY   ║\n");
        fprintf(stdout, "╠══════════════════════════════════════════════════════════╣\n");
        fprintf(stdout, ANSI_RESET);
        
        int grand_total_items = 0;
        double grand_total_value = 0.0;
        
        shm_lock(h.sem_id);
        for (int i = 0; i < st->P; ++i) {
            int total_sold = 0;
            for (int c = 0; c < CASHIERS; ++c) {
                total_sold += st->sold_by_cashier[c][i];
            }
            if (total_sold > 0) {
                double value = total_sold * st->produkty[i].cena;
                fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  P%02d: %-25s %4d × %6.2f = " ANSI_BOLD "%8.2f zł" ANSI_RESET " " COLOR_KIEROWNIK "║" ANSI_RESET "\n", 
                        i, st->produkty[i].nazwa, total_sold, st->produkty[i].cena, value);
                grand_total_items += total_sold;
                grand_total_value += value;
            }
        }
        shm_unlock(h.sem_id);
        
        if (grand_total_items == 0) {
            fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  (brak sprzedazy)                                        " COLOR_KIEROWNIK "║" ANSI_RESET "\n");
        }
        
        fprintf(stdout, COLOR_KIEROWNIK "╠══════════════════════════════════════════════════════════╣" ANSI_RESET "\n");
        fprintf(stdout, COLOR_KIEROWNIK "║" ANSI_RESET "  " ANSI_BOLD ANSI_GREEN "SUMA: %4d szt., wartość: %12.2f zł" ANSI_RESET "             " COLOR_KIEROWNIK "║" ANSI_RESET "\n", 
                grand_total_items, grand_total_value);
        fprintf(stdout, COLOR_KIEROWNIK "╚══════════════════════════════════════════════════════════╝" ANSI_RESET "\n");
    }

    /* Wyswietl statystyki testowe */
    if (g_test_mode) {
        print_test_stats(st);
    }

    /* Poczekaj na dzieci */
    int status;
    int children_reaped = 0;
    while (wait(&status) > 0) {
        children_reaped++;
    }
    LOGF("kierownik", "Zakonczono %d procesow potomnych.", children_reaped);

    if (fifo_fd >= 0) close(fifo_fd);
    unlink(CTRL_FIFO_PATH);

    ipc_detach_or_die(st);
    ipc_destroy_or_die(&h, P);

    LOGF("kierownik", "Symulacja zakonczona pomyslnie.");
    return 0;
}
