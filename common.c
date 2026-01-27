#include "common.h"

/* =========================
 *  ftok() i plik klucza
 * ========================= */

void ensure_ipc_key_file_or_die(void) {
    /*
     * ftok() wymaga istniejącej ścieżki.
     * Tworzymy mały plik "key file" o minimalnych prawach.
     */
    int fd = open(IPC_KEY_FILE, O_CREAT | O_RDWR, IPC_PERMS_MIN);
    if (fd == -1) DIE_PERROR("open(IPC_KEY_FILE)");
    /* TODO: można zapisać do niego PID/znacznik wersji. */
    close(fd);
}

key_t bakery_ftok_or_die(int proj_id) {
    key_t k = ftok(IPC_KEY_FILE, proj_id);
    if (k == (key_t)-1) DIE_PERROR("ftok");
    return k;
}

/* =========================
 *  IPC create/attach/destroy
 * ========================= */

static void init_state_defaults(BakeryState* st) {
    memset(st, 0, sizeof(*st));
    st->store_open = 1;
    st->inventory_mode = 0;
    st->evacuated = 0;

    for (int c = 0; c < CASHIERS; ++c) {
        st->cashier_open[c] = (c == 0) ? 1 : 0;     /* zawsze min. 1 działa */
        st->cashier_accepting[c] = st->cashier_open[c];
    }
}

/* Uwaga: tworzymy segment SHM o stałej wielkości BakeryState (wstępna wersja). */
void ipc_create_or_die(IpcHandles* out, int P) {
    if (!out) {
        errno = EINVAL;
        DIE_PERROR("ipc_create_or_die(out==NULL)");
    }

    ensure_ipc_key_file_or_die();

    /* SHM */
    key_t shm_key = bakery_ftok_or_die(0x41);
    int shm_id = shmget(shm_key, sizeof(BakeryState), IPC_CREAT | IPC_EXCL | IPC_PERMS_MIN);
    if (shm_id == -1) DIE_PERROR("shmget");

    /* SEM */
    key_t sem_key = bakery_ftok_or_die(0x42);
    int sem_n = sem_count_for_P(P);
    int sem_id = semget(sem_key, sem_n, IPC_CREAT | IPC_EXCL | IPC_PERMS_MIN);
    if (sem_id == -1) DIE_PERROR("semget");

    /* MSG (3 kolejki) */
    for (int i = 0; i < CASHIERS; ++i) {
        key_t msg_key = bakery_ftok_or_die(0x50 + i);
        int msg_id = msgget(msg_key, IPC_CREAT | IPC_EXCL | IPC_PERMS_MIN);
        if (msg_id == -1) DIE_PERROR("msgget");
        out->msg_id[i] = msg_id;
    }

    out->shm_id = shm_id;
    out->sem_id = sem_id;

    /* Podłącz SHM i zainicjalizuj */
    BakeryState* st = (BakeryState*)shmat(shm_id, NULL, 0);
    CHECK_PTR(st, "shmat (create)");

    init_state_defaults(st);

    /* TODO: w bakery po walidacji uzupełnic P/N/Tp/Tk/ceny/Ki i conveyors[].capacity */
    /* TODO: rozwazyc init losowego seed w bakery (srand). */

    /* Zainicjalizuj semafory */
    union semun arg;
    unsigned short* vals = calloc((size_t)sem_n, sizeof(unsigned short));
    if (!vals) DIE_PERROR("calloc sem vals");

    /* SEM_STORE_SLOTS i SEM_SHM_GLOBAL uzupełni bakery po ustawieniu N */
    vals[SEM_STORE_SLOTS] = 0; /* TODO: set N */
    vals[SEM_SHM_GLOBAL]  = 1;

    /* Semafory per produkt: mutex=1, empty=Ki, full=0 (tu 0, Ki w bakery) */
    for (int i = 0; i < P; ++i) {
        vals[SEM_CONV_MUTEX(i)] = 1;
        vals[SEM_CONV_EMPTY(i)] = 0; /* TODO: set Ki[i] */
        vals[SEM_CONV_FULL(i)]  = 0;
    }

    arg.array = vals;
    CHECK_SYS(semctl(sem_id, 0, SETALL, arg), "semctl(SETALL)");

    free(vals);

    CHECK_SYS(shmdt(st), "shmdt (create)");
}

void ipc_attach_or_die(const IpcHandles* h, BakeryState** out_state) {
    if (!h || !out_state) {
        errno = EINVAL;
        DIE_PERROR("ipc_attach_or_die");
    }
    BakeryState* st = (BakeryState*)shmat(h->shm_id, NULL, 0);
    CHECK_PTR(st, "shmat (attach)");
    *out_state = st;
}

void ipc_detach_or_die(BakeryState* state) {
    if (!state) return;
    CHECK_SYS(shmdt(state), "shmdt");
}

void ipc_destroy_or_die(const IpcHandles* h, int P) {
    (void)P; 
    if (!h) return;

    /* Kolejki */
    for (int i = 0; i < CASHIERS; ++i) {
        if (h->msg_id[i] != -1) {
            CHECK_SYS(msgctl(h->msg_id[i], IPC_RMID, NULL), "msgctl(IPC_RMID)");
        }
    }

    /* Semafory */
    if (h->sem_id != -1) {
        CHECK_SYS(semctl(h->sem_id, 0, IPC_RMID), "semctl(IPC_RMID)");
    }

    /* SHM */
    if (h->shm_id != -1) {
        CHECK_SYS(shmctl(h->shm_id, IPC_RMID, NULL), "shmctl(IPC_RMID)");
    }

    /* TODO: usunac CTRL_FIFO_PATH jeśli używane */
    /* TODO: usunac IPC_KEY_FILE jeśli chce (unlink) */
}

/* =========================
 *  Semafory: P/V
 * ========================= */

static int semop_or_die(int sem_id, unsigned short sem_num, short delta, int flags) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op  = delta;
    op.sem_flg = (short)flags;

    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;   /* przerwane sygnałem -> ponów */
        DIE_PERROR("semop");
    }
}

void sem_P(int sem_id, int sem_num) {
    semop_or_die(sem_id, (unsigned short)sem_num, -1, 0);
}

int sem_P_nowait(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = (unsigned short)sem_num;
    op.sem_op  = -1;
    op.sem_flg = IPC_NOWAIT;

    if (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) return -1;
        return -1;
    }
    return 0;
}

void sem_V(int sem_id, int sem_num) {
    semop_or_die(sem_id, (unsigned short)sem_num, +1, 0);
}

void shm_lock(int sem_id) {
    return sem_P(sem_id, SEM_SHM_GLOBAL);
}
void shm_unlock(int sem_id) {
    return sem_V(sem_id, SEM_SHM_GLOBAL);
}

/* =========================
 *  Losowanie / czas
 * ========================= */

int rand_between(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == b) return a;
    int r = rand();
    return a + (r % (b - a + 1));
}

void msleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* TODO: jeśli chce reagować na sygnały, sprawdic flagi globalne */
    }
}

/* =========================
 *  Walidacja konfiguracji
 * ========================= */

int validate_config(int P, int N, int open_hour, int close_hour, const int* Ki, const Product* produkty) {
    if (P <= 10 || P > MAX_P) return 0;
    if (N <= 0) return 0;
    if (N % 3 != 0) return 0;
    if (open_hour < 0 || open_hour > 23) return 0;
    if (close_hour < 0 || close_hour > 23) return 0;
    if (open_hour >= close_hour) return 0;
    if (!Ki || !produkty) return 0;

    for (int i = 0; i < P; ++i) {
        if (Ki[i] <= 0 || Ki[i] > MAX_KI) return 0;
        if (produkty[i].cena <= 0.0) return 0;
    }
    return 1;
}

/* =========================
 *  Sygnały
 * ========================= */

void install_signal_handlers_or_die(void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);

    /* TODO: rozważyc SA_RESTART vs własna obsługa EINTR */
    sa.sa_flags = 0;

    CHECK_SYS(sigaction(SIG_EVAC, &sa, NULL), "sigaction(SIG_EVAC)");
    CHECK_SYS(sigaction(SIG_INV, &sa, NULL), "sigaction(SIG_INV)");

    /* sprzątanie po Ctrl+C */
    CHECK_SYS(sigaction(SIGINT, &sa, NULL), "sigaction(SIGINT)");
    CHECK_SYS(sigaction(SIGTERM, &sa, NULL), "sigaction(SIGTERM)");
}
