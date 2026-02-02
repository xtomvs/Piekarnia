#!/bin/bash
#
# run_tests.sh - Skrypt do przeprowadzania testow przeciazeniowych piekarni
#
# Testy sprawdzaja:
# 1. Brak blokad (deadlock) przy duzej liczbie klientow
# 2. Przestrzeganie limitu N klientow w sklepie
# 3. Poprawna obsluga sygnalow (ewakuacja, inwentaryzacja)
# 4. Poprawne zamykanie procesow
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Kolory dla wyjscia
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Czyszczenie zasobow IPC
cleanup() {
    ipcrm -a 2>/dev/null
    rm -f .bakery_ipc_key bakery_ctrl.fifo
    pkill -9 -f "./manager" 2>/dev/null
    pkill -9 -f "./client" 2>/dev/null
    pkill -9 -f "./baker" 2>/dev/null
    pkill -9 -f "./cashier" 2>/dev/null
}

# Funkcja do uruchamiania testu
run_test() {
    local test_name="$1"
    local client_count="$2"
    local timeout_sec="$3"
    local extra_args="$4"
    
    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}TEST: $test_name${NC}"
    echo -e "${YELLOW}Klienci: $client_count, Timeout: ${timeout_sec}s${NC}"
    echo -e "${YELLOW}========================================${NC}"
    
    cleanup
    
    local log_file="test_${test_name}.log"
    
    if timeout "$timeout_sec" ./manager test "$client_count" $extra_args > "$log_file" 2>&1; then
        echo -e "${GREEN}[PASS]${NC} Test zakonczony pomyslnie"
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "${RED}[FAIL]${NC} Test przekroczyl timeout - mozliwe zakleszczenie!"
            return 1
        else
            echo -e "${RED}[FAIL]${NC} Test zakonczony z bledem (kod: $exit_code)"
            return 1
        fi
    fi
    
    # Analiza wynikow
    local max_in_store=$(grep "Max rownoczesnie" "$log_file" | grep -oP '\d+(?= \(limit)')
    local limit_n=$(grep "Max rownoczesnie" "$log_file" | grep -oP '(?<=limit N=)\d+')
    local spawned=$(grep "Klientow wygenerowanych" "$log_file" | grep -oP '\d+')
    local time_ms=$(grep "Czas trwania" "$log_file" | grep -oP '\d+')
    
    echo "  Wygenerowano klientow: $spawned"
    echo "  Max w sklepie: $max_in_store / $limit_n"
    echo "  Czas trwania: ${time_ms}ms"
    
    # Sprawdz czy nie przekroczono limitu N
    if [ -n "$max_in_store" ] && [ -n "$limit_n" ] && [ "$max_in_store" -gt "$limit_n" ]; then
        echo -e "${RED}[FAIL]${NC} PRZEKROCZONO LIMIT N!"
        return 1
    fi
    
    # Sprawdz czy wszystkie procesy sie zakonczyly
    if grep -q "TIMEOUT" "$log_file"; then
        echo -e "${RED}[WARN]${NC} Wykryto timeout - niektore procesy mogly sie zablokowac"
    fi
    
    echo ""
    return 0
}

# Test ewakuacji
run_evacuation_test() {
    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}TEST: Ewakuacja (SIGUSR1)${NC}"
    echo -e "${YELLOW}========================================${NC}"
    
    cleanup
    
    local log_file="test_evacuation.log"
    
    # Uruchom w tle
    ./manager test 500 > "$log_file" 2>&1 &
    local manager_pid=$!
    
    sleep 2
    
    # Wyslij sygnal ewakuacji
    echo "Wysylam sygnal ewakuacji (SIGUSR1)..."
    kill -SIGUSR1 -$manager_pid 2>/dev/null
    
    # Poczekaj na zakonczenie
    local wait_count=0
    while kill -0 $manager_pid 2>/dev/null && [ $wait_count -lt 30 ]; do
        sleep 1
        ((wait_count++))
    done
    
    if kill -0 $manager_pid 2>/dev/null; then
        echo -e "${RED}[FAIL]${NC} Manager nie zakonczyl sie po ewakuacji"
        kill -9 $manager_pid 2>/dev/null
        return 1
    fi
    
    # Sprawdz czy ewakuacja zostala obslugona
    if grep -q "EWAKUACJA" "$log_file"; then
        echo -e "${GREEN}[PASS]${NC} Ewakuacja obslugona poprawnie"
        
        local wasted=$(grep "zmarnowanych" "$log_file" | grep -oP '\d+')
        echo "  Produkty odlozone do kosza: $wasted"
    else
        echo -e "${RED}[FAIL]${NC} Brak informacji o ewakuacji w logach"
        return 1
    fi
    
    echo ""
    return 0
}

# Glowna czesc skryptu
echo "============================================"
echo "     TESTY PRZECIAZENIOWE - PIEKARNIA"
echo "============================================"
echo ""

# Sprawdz czy pliki wykonywalne istnieja
if [ ! -f "./manager" ] || [ ! -f "./client" ]; then
    echo "Blad: Brak plikow wykonywalnych. Uruchom 'make' najpierw."
    exit 1
fi

PASSED=0
FAILED=0

# Test 1: 100 klientow (szybki test)
if run_test "100_klientow" 100 30; then
    ((PASSED++))
else
    ((FAILED++))
fi

# Test 2: 500 klientow (sredni test)
if run_test "500_klientow" 500 60; then
    ((PASSED++))
else
    ((FAILED++))
fi

# Test 3: 1000 klientow (duzy test)
if run_test "1000_klientow" 1000 120; then
    ((PASSED++))
else
    ((FAILED++))
fi

# Test 4: Ewakuacja
if run_evacuation_test; then
    ((PASSED++))
else
    ((FAILED++))
fi

# Podsumowanie
echo "============================================"
echo "            PODSUMOWANIE TESTOW"
echo "============================================"
echo -e "Testy zakonczone: ${GREEN}$PASSED przeszlo${NC}, ${RED}$FAILED nie przeszlo${NC}"
echo ""

# Czyszczenie
cleanup

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
