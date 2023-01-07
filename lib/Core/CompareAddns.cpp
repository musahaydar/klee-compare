// Additions for KLEE Compare

#include <stdio.h>

#include "klee/klee.h"

// Intrinsic defined in include/klee/klee.h
// Used in the modified POSIX runtime to dump syscall data so we can compare
// Excepts a c-string
void klee_compare_dump(const char *data) {
    printf("KLEE DUMPING: %s", data);
}