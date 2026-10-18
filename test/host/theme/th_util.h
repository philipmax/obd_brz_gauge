#pragma once
// Shared helpers for the theme engine host tests.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "cJSON.h"

extern int th_fail;
extern int th_checks;

#define TH_CHECK(cond)                                                        \
    do {                                                                      \
        th_checks++;                                                          \
        if (!(cond)) {                                                        \
            th_fail++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                     \
    } while (0)

// Reads a whole file (NUL-terminated); NULL on failure.
char *th_read_file(const char *path, size_t *len);

// Reads and parses a JSON file; NULL on failure (message printed).
cJSON *th_load_json(const char *path);

// Prints the summary; returns the process exit code.
int th_report(const char *name);
