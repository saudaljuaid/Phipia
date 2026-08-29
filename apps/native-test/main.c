/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <sapote/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local unsigned long tls_value = 17U;

static void *thread_probe(void *argument)
{
    const unsigned long expected = (unsigned long)(uintptr_t)argument;
    volatile double vector_value = (double)expected * 1.5;

    tls_value = expected;
    for (unsigned int iteration = 0U; iteration < 64U; ++iteration) {
        (void)sapote_sleep_until(sapote_monotonic_ns() + 100000U);
        if (tls_value != expected || vector_value != (double)expected * 1.5) {
            return (void *)(uintptr_t)1U;
        }
    }
    return NULL;
}

int main(int argc, char **argv, char **environment)
{
    pthread_t first;
    pthread_t second;
    void *first_result = (void *)(uintptr_t)1U;
    void *second_result = (void *)(uintptr_t)1U;
    char *memory;
    FILE *file;

    if (sapote_syscall0(SAPOTE_SYS_ABI_VERSION) != SAPOTE_ABI_VERSION ||
        argc < 1 || argv == NULL || environment == NULL) {
        return 10;
    }
    memory = malloc(8192U);
    if (memory == NULL) return 11;
    (void)memset(memory, 0x5A, 8192U);
    if (memory[0] != 0x5A || memory[8191] != 0x5A) return 12;
    free(memory);
    file = fopen("FOUND.TXT", "w+");
    if (file == NULL || fputs("native ABI v1\n", file) == EOF ||
        fflush(file) != 0 || fseek(file, 0L, SEEK_SET) != 0) return 13;
    {
        char line[32];
        if (fgets(line, sizeof(line), file) == NULL ||
            strcmp(line, "native ABI v1\n") != 0 || fclose(file) != 0) {
            return 14;
        }
    }
    if (pthread_create(&first, NULL, thread_probe,
            (void *)(uintptr_t)101U) != 0 ||
        pthread_create(&second, NULL, thread_probe,
            (void *)(uintptr_t)202U) != 0 ||
        pthread_join(first, &first_result) != 0 ||
        pthread_join(second, &second_result) != 0 || first_result != NULL ||
        second_result != NULL || tls_value != 17U) {
        return 15;
    }
    printf("SAPOTE NATIVE PASS argc=%d app=%s\n", argc, argv[0]);
    return 0;
}
