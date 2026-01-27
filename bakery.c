#include "common.h"

/*
 * bakery.c – bootstrap / punkt wejścia projektu.
 */

int main(int argc, char** argv) {
    (void)argc;
    /* Przekazujemy argumenty dalej do manager */
    execv("./manager", argv);
    perror("execv(./manager)");
    return 127;
}
