/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <sapote/event.h>
#include <sapote/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local unsigned long tls_value = 17U;

int native_state_round_trip(uint64_t deadline_ns, uint64_t seed);
extern const uint8_t native_initial_fpu_state[512];

static int initial_state_is_clean(void)
{
    const uint8_t *const state = native_initial_fpu_state;
    uint32_t mxcsr;

    (void)memcpy(&mxcsr, state + 24U, sizeof(mxcsr));
    if (state[0] != 0x7FU || state[1] != 0x03U || state[4] != 0U ||
        mxcsr != UINT32_C(0x1F80)) {
        return 0;
    }
    for (size_t index = 160U; index < 416U; ++index) {
        if (state[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void *thread_probe(void *argument)
{
    const unsigned long expected = (unsigned long)(uintptr_t)argument;

    tls_value = expected;
    for (unsigned int iteration = 0U; iteration < 64U; ++iteration) {
        if (native_state_round_trip(sapote_monotonic_ns() + 100000U,
                expected * 257U + iteration) != 0 || tls_value != expected) {
            return (void *)(uintptr_t)1U;
        }
    }
    return NULL;
}

static int memory_and_pointer_probes(void)
{
    struct sapote_memory_map_response split = {0U, 0U, 0U, 0U};
    struct sapote_memory_map_response mappings[16];
    struct sapote_memory_map_response ignored = {0U, 0U, 0U, 0U};
    const struct sapote_memory_map_request bad_flags = {
        sizeof(bad_flags), SAPOTE_ABI_VERSION, SAPOTE_ABI_PAGE_SIZE, 0U,
        UINT32_C(0x80000000), 0U
    };
    size_t mapping_count = 0U;
    long exhausted = 0;

    if (sapote_syscall2(SAPOTE_SYS_MEMORY_MAP,
            (uint64_t)(uintptr_t)&bad_flags,
            (uint64_t)(uintptr_t)&ignored) != -SAPOTE_EINVAL ||
        sapote_random((void *)(uintptr_t)UINT64_C(0x12345000), 1U) !=
            -SAPOTE_EFAULT ||
        sapote_memory_allocate(2U * SAPOTE_ABI_PAGE_SIZE,
            SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE, &split) != 0) {
        return 20;
    }
    {
        volatile uint8_t *edge = (volatile uint8_t *)(uintptr_t)
            (split.address + SAPOTE_ABI_PAGE_SIZE - 1U);

        *edge = UINT8_C(0xA5);
        if (sapote_memory_release(split.address + SAPOTE_ABI_PAGE_SIZE,
                SAPOTE_ABI_PAGE_SIZE) != 0 ||
            sapote_random((void *)(uintptr_t)(split.address +
                SAPOTE_ABI_PAGE_SIZE - 1U), 2U) != -SAPOTE_EFAULT ||
            *edge != UINT8_C(0xA5) ||
            sapote_memory_release(split.address, SAPOTE_ABI_PAGE_SIZE) != 0) {
            return 21;
        }
    }
    while (mapping_count < sizeof(mappings) / sizeof(mappings[0])) {
        const long status = sapote_memory_allocate(2U * 1024U * 1024U,
            SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE,
            &mappings[mapping_count]);

        if (status < 0) {
            exhausted = status;
            break;
        }
        ++mapping_count;
    }
    if (exhausted != -SAPOTE_ENOMEM || mapping_count == 0U) {
        return 22;
    }
    while (mapping_count != 0U) {
        --mapping_count;
        if (sapote_memory_release(mappings[mapping_count].address,
                mappings[mapping_count].length) != 0) {
            return 23;
        }
    }
    return 0;
}

static int file_and_handle_probes(void)
{
    static const char replacement[] = "replacement";
    struct sapote_volume_space space = {0U, 0U, 0U, 0U, 0U, 0U};
    struct sapote_directory_entry entry;
    char bytes[sizeof(replacement)];
    long file;
    long duplicate;
    long directory;
    int found = 0;

    if (sapote_file_open(SAPOTE_VOLUME_DATA, "../ESCAPE.TXT",
            SAPOTE_OPEN_READ) != -SAPOTE_EINVAL ||
        sapote_file_open(SAPOTE_VOLUME_SYSTEM, "../NATIVET.APP",
            SAPOTE_OPEN_READ) != -SAPOTE_EINVAL ||
        sapote_syscall0(SAPOTE_SYS_STREAM_OPEN) != -SAPOTE_EACCES ||
        sapote_syscall0(UINT64_C(0xFFFF)) != -SAPOTE_ENOSYS) {
        return 24;
    }
    if (sapote_path_mkdir(SAPOTE_VOLUME_DATA, "TMP") != 0) {
        return 25;
    }
    file = sapote_file_open(SAPOTE_VOLUME_DATA, "TMP/A.TXT",
        SAPOTE_OPEN_READ | SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE |
            SAPOTE_OPEN_TRUNCATE);
    if (file < 0 || sapote_timer_set((sapote_handle_t)file,
            sapote_monotonic_ns()) != -SAPOTE_EBADF) {
        return 26;
    }
    duplicate = sapote_handle_duplicate((sapote_handle_t)file);
    if (duplicate < 0 || sapote_handle_close((sapote_handle_t)file) != 0 ||
        sapote_file_read((sapote_handle_t)file, bytes, 1U) != -SAPOTE_ESTALE ||
        sapote_handle_close((sapote_handle_t)file) != -SAPOTE_ESTALE ||
        sapote_file_write((sapote_handle_t)duplicate, "abcdef", 6U) != 6 ||
        sapote_handle_close((sapote_handle_t)duplicate) != 0) {
        return 27;
    }
    if (sapote_path_truncate(SAPOTE_VOLUME_DATA, "TMP/A.TXT", 3U) != 0 ||
        sapote_path_rename(SAPOTE_VOLUME_DATA, "TMP/A.TXT", "TMP/B.TXT") !=
            0) {
        return 28;
    }
    file = sapote_file_open(SAPOTE_VOLUME_DATA, "TMP/C.TXT",
        SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE | SAPOTE_OPEN_TRUNCATE);
    if (file < 0 || sapote_file_write((sapote_handle_t)file, replacement,
            sizeof(replacement) - 1U) != (long)(sizeof(replacement) - 1U) ||
        sapote_handle_close((sapote_handle_t)file) != 0 ||
        sapote_path_replace(SAPOTE_VOLUME_DATA, "TMP/C.TXT", "TMP/B.TXT") !=
            0) {
        return 29;
    }
    file = sapote_file_open(SAPOTE_VOLUME_DATA, "TMP/B.TXT", SAPOTE_OPEN_READ);
    if (file < 0 || sapote_file_read((sapote_handle_t)file, bytes,
            sizeof(bytes)) != (long)(sizeof(replacement) - 1U) ||
        memcmp(bytes, replacement, sizeof(replacement) - 1U) != 0 ||
        sapote_handle_close((sapote_handle_t)file) != 0) {
        return 30;
    }
    directory = sapote_directory_open(SAPOTE_VOLUME_DATA, "TMP");
    if (directory < 0) {
        return 31;
    }
    for (;;) {
        const long status = sapote_directory_read((sapote_handle_t)directory,
            &entry);

        if (status < 0) {
            return 32;
        }
        if (status == 0) {
            break;
        }
        if (entry.name_length == 5U &&
            memcmp(entry.name, "B.TXT", 5U) == 0) {
            found = 1;
        }
    }
    if (!found) return 331;
    if (sapote_handle_close((sapote_handle_t)directory) != 0) return 332;
    if (sapote_volume_space(SAPOTE_VOLUME_DATA, &space) != 0) return 333;
    if (space.total_bytes == 0U || space.free_bytes >= space.total_bytes) {
        return 334;
    }
    if (sapote_volume_sync(SAPOTE_VOLUME_DATA) != 0) return 335;
    if (sapote_path_unlink(SAPOTE_VOLUME_DATA, "TMP/B.TXT") != 0) return 336;
    if (sapote_path_unlink(SAPOTE_VOLUME_DATA, "TMP") != 0) return 337;
    if (sapote_volume_sync(SAPOTE_VOLUME_DATA) != 0) return 338;
    return 0;
}

static int timer_probe(void)
{
    struct sapote_wait_item item;
    const long timer = sapote_timer_create();
    uint64_t now;

    if (timer < 0) {
        return 34;
    }
    now = sapote_monotonic_ns();
    item = (struct sapote_wait_item){(sapote_handle_t)timer,
        SAPOTE_WAIT_SIGNALED, 0U};
    if (sapote_timer_set((sapote_handle_t)timer, now + UINT64_C(1000000)) != 0 ||
        sapote_wait(&item, 1U, now + UINT64_C(20000000)) != 1 ||
        item.ready != SAPOTE_WAIT_SIGNALED) {
        return 35;
    }
    now = sapote_monotonic_ns();
    item.ready = 0U;
    if (sapote_timer_set((sapote_handle_t)timer, now + UINT64_C(1000000000)) !=
            0 || sapote_wait(&item, 1U, now) != -SAPOTE_ETIMEDOUT ||
        sapote_cancel((sapote_handle_t)timer) != 0 ||
        sapote_handle_close((sapote_handle_t)timer) != 0) {
        return 36;
    }
    return 0;
}

int main(int argc, char **argv, char **environment)
{
    static const char expected_resource[] = "Sapote immutable resource\n";
    pthread_t first;
    pthread_t second;
    void *first_result = (void *)(uintptr_t)1U;
    void *second_result = (void *)(uintptr_t)1U;
    char *memory;
    FILE *file;
    char resource[sizeof(expected_resource)];
    long resource_handle;
    int probe;

    if (sapote_syscall0(SAPOTE_SYS_ABI_VERSION) != SAPOTE_ABI_VERSION ||
        argc < 1 || argv == NULL || environment == NULL) {
        return 10;
    }
    if (!initial_state_is_clean()) {
        return 11;
    }
    memory = malloc(8192U);
    if (memory == NULL) return 12;
    (void)memset(memory, 0x5A, 8192U);
    if (memory[0] != 0x5A || memory[8191] != 0x5A) return 13;
    free(memory);
    probe = memory_and_pointer_probes();
    if (probe != 0) return probe;
    probe = file_and_handle_probes();
    if (probe != 0) return probe;
    probe = timer_probe();
    if (probe != 0) return probe;
    resource_handle = sapote_file_open(SAPOTE_VOLUME_SYSTEM, "RESOURCE.TXT",
        SAPOTE_OPEN_READ);
    if (resource_handle < 0 || sapote_file_read((sapote_handle_t)resource_handle,
            resource, sizeof(resource)) !=
            (long)(sizeof(expected_resource) - 1U) ||
        memcmp(resource, expected_resource, sizeof(expected_resource) - 1U) != 0 ||
        sapote_file_read((sapote_handle_t)resource_handle, resource, 1U) != 0 ||
        sapote_handle_close((sapote_handle_t)resource_handle) < 0) {
        return 37;
    }
    file = fopen("FOUND.TXT", "w+");
    if (file == NULL || fputs("native ABI v1\n", file) == EOF ||
        fflush(file) != 0 || fseek(file, 0L, SEEK_SET) != 0) return 38;
    {
        char line[32];
        if (fgets(line, sizeof(line), file) == NULL ||
            strcmp(line, "native ABI v1\n") != 0 || fclose(file) != 0) {
            return 39;
        }
    }
    if (pthread_create(&first, NULL, thread_probe,
            (void *)(uintptr_t)101U) != 0 ||
        pthread_create(&second, NULL, thread_probe,
            (void *)(uintptr_t)202U) != 0 ||
        pthread_join(first, &first_result) != 0 ||
        pthread_join(second, &second_result) != 0 || first_result != NULL ||
        second_result != NULL || tls_value != 17U) {
        return 40;
    }
    printf("SAPOTE REFUSAL capability EACCES stale ESTALE pointer EFAULT "
        "traversal EINVAL exhaustion ENOMEM\n");
    printf("SAPOTE FILE create seek truncate rename replace sync unlink PASS\n");
    printf("SAPOTE STATE general FS x87 SSE PASS\n");
    printf("SAPOTE NATIVE PASS argc=%d app=%s\n", argc, argv[0]);
    return 0;
}
