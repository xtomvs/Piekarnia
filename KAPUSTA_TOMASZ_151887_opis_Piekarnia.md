# Temat 12 - Piekarnia

## Repozytorium
https://github.com/xtomvs/Piekarnia

## Opis projektu

Piekarnia produkuje P roznych produktow (P>10), kazdy w innej cenie i na biezaco sprzedaje je w samoobslugowym sklepie firmowym. Produkty bezposrednio po wypieku (losowa liczba sztuk roznych produktow co okreslony czas) trafiaja do sprzedazy w sklepie – kazdy rodzaj produktu Pi na oddzielny podajnik. Kazdy podajnik moze przetransportowac w danej chwili maksymalnie Ki sztuk pieczywa. Pieczywo z danego podajnika musi byc pobieranie w sklepie dokladnie w takiej kolejnosci jak zostalo polozone na tym podajniku w piekarni.

### Zasady dzialania piekarni:
- Piekarnia/sklep jest czynny w godzinach od Tp do Tk
- W sklepie w danej chwili moze sie znajdowac co najwyzej N klientow (pozostali czekaja przed wejsciem)
- W sklepie sa 3 stanowiska kasowe, zawsze dziala min. 1 stanowisko kasowe
- Na kazdych K (K=N/3) klientow znajdujacych sie na terenie supermarketu powinno przypadac min. 1 czynne stanowisko kasowe
- Jesli liczba klientow jest mniejsza niz 2*N/3, to jedna z 3 kas zostaje zamknieta
- Jesli w kolejce do kasy czekali klienci (przed ogloszeniem decyzji o jej zamknieciu) to powinni zostac obsluzeni przez te kase

### Obsluga sygnalow:
- **SIGUSR1 (ewakuacja)**: Klienci natychmiast przerywaja zakupy i opuszczaja piekarnie omijajac kasy – pobrany juz z podajnikow towar odkladaja do kosza przy kasach
- **SIGUSR2 (inwentaryzacja)**: Klienci kontynuuja zakupy normalnie do zamkniecia piekarni/sklepu. Po zamknieciu sklepu, kazda kasa robi podsumowanie sprzedanych produktow

## Struktura projektu

```
piekarnia/
├── manager.c      # Kierownik - glowna petla, inicjalizacja IPC, polityka kas
├── baker.c        # Piekarz - produkcja pieczywa
├── cashier.c      # Kasjer - obsluga klientow przy kasie
├── client.c       # Klient - zakupy w sklepie
├── common.c       # Wspolne funkcje IPC, semafory, walidacja
├── common.h       # Wspolne definicje, struktury danych
├── Makefile       # Budowanie projektu
├── run_tests.sh   # Skrypt do testow przeciazeniowych
└── README.md
```

## Wykorzystane mechanizmy IPC

### 1. Pamieci dzielona (Shared Memory)
- Przechowuje globalny stan piekarni (`BakeryState`)
- Informacje o produktach, podajnikach, kasach, liczbie klientow
- Statystyki sprzedazy i produkcji

### 2. Semafory (System V)
- `SEM_STORE_SLOTS`: Ograniczenie liczby klientow w sklepie (N)
- `SEM_SHM_GLOBAL`: Mutex dla operacji na pamieci dzielonej
- `SEM_CONV_MUTEX(i)`: Mutex dla podajnika produktu i
- `SEM_CONV_EMPTY(i)`: Licznik wolnych miejsc na podajniku i
- `SEM_CONV_FULL(i)`: Licznik produktow na podajniku i

### 3. Kolejki komunikatow (Message Queues)
- 3 kolejki (jedna na kase) do przekazywania koszyka klienta kasjerowi
- Struktura `ClientMsg` z lista produktow i ilosci

### 4. Sygnaly
- SIGUSR1 - ewakuacja
- SIGUSR2 - inwentaryzacja
- SIGINT/SIGTERM - zamkniecie

## Budowanie

```bash
make clean && make
```

## Uruchamianie

### Tryb normalny (wg godzin):
```bash
./manager
```

### Tryb testowy (okreslona liczba klientow):
```bash
./manager test 100    # 100 klientow
./manager test 500    # 500 klientow
./manager test 1000   # 1000 klientow
```

### Tryb stress (maksymalne obciazenie):
```bash
./manager stress
```

## Testy przeciazeniowe

### Uruchomienie testow:
```bash
./run_tests.sh
```

### Opis testow:

| Test | Klienci | Timeout | Co sprawdza |
|------|---------|---------|-------------|
| 100_klientow | 100 | 30s | Podstawowa funkcjonalnosc |
| 500_klientow | 500 | 60s | Srednie obciazenie |
| 1000_klientow | 1000 | 120s | Duze obciazenie, brak zakleszczen |
| Ewakuacja | ~200 | 30s | Obsluga SIGUSR1 |

### Wyniki przykladowe:

```
============================================
     TESTY PRZECIAZENIOWE - PIEKARNIA
============================================

TEST: 100_klientow
[PASS] Test zakonczony pomyslnie
  Wygenerowano klientow: 100
  Max w sklepie: 74 / 100
  Czas trwania: 1937ms

TEST: 500_klientow
[PASS] Test zakonczony pomyslnie
  Wygenerowano klientow: 500
  Max w sklepie: 73 / 100
  Czas trwania: 6341ms

TEST: 1000_klientow
[PASS] Test zakonczony pomyslnie
  Wygenerowano klientow: 1000
  Max w sklepie: 77 / 100
  Czas trwania: 11364ms

TEST: Ewakuacja (SIGUSR1)
[PASS] Ewakuacja obslugona poprawnie
  Produkty odlozone do kosza: 10

PODSUMOWANIE TESTOW
Testy zakonczone: 4 przeszlo, 0 nie przeszlo
```

## Zapobieganie zakleszczeniom i blokadom

### 1. Kolejnosc semaforow
- Zawsze ten sam porzadek blokowania: STORE_SLOTS -> SHM_GLOBAL -> CONV_MUTEX
- Unikanie cyklicznego oczekiwania

### 2. Blokujace oczekiwanie z obsluga sygnalow
```c
static int sem_P_interruptible(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = (unsigned short)sem_num;
    op.sem_op  = -1;
    op.sem_flg = 0;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) {
            if (g_stop || g_evac) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
```

### 3. Non-blocking operacje na podajnikach
- Klient uzywa `sem_P_nowait()` do pobierania produktow
- Jesli brak towaru, nie blokuje sie - idzie dalej

### 4. Timeout przy zamykaniu
- Manager czeka max 60s na wyjscie klientow
- Po timeoucie wymusza zamkniecie

## Wykorzystane funkcje systemowe

| Kategoria | Funkcje |
|-----------|---------|
| Procesy | `fork()`, `exec()`, `exit()`, `wait()`, `waitpid()` |
| Sygnaly | `sigaction()`, `kill()` |
| Semafory | `semget()`, `semctl()`, `semop()` |
| Pam. dzielona | `shmget()`, `shmat()`, `shmdt()`, `shmctl()` |
| Kolejki | `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()` |
| FIFO | `mkfifo()`, `open()`, `read()` |
| Czas | `clock_gettime()`, `nanosleep()` |

## Autor
Tomasz Kapusta, 151887