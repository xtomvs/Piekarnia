#include "common.h"

/*
 * bakery.c – bootstrap / punkt wejścia projektu.
 *
 * Wymaganie mówi o osobnym programie "kierownika".
 * Dlatego właściwa logika kierownika jest w pliku manager.c (binarka: ./manager),
 * a ten plik jest cienkim wrapperem uruchamiającym kierownika.
 *
 * TODO: Jeśli prowadzący nie chce wrappera – usuń bakery.c i uruchamiaj ./manager jako główny.
 */

int main(int argc, char** argv) {
    (void)argc;
    /* Przekazujemy argumenty dalej do manager */
    execv("./manager", argv);
    perror("execv(./manager)");
    return 127;
}
