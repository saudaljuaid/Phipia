/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <sapote/event.h>
#include <sapote/runtime.h>
#include <sapote/window.h>
#include <stdint.h>

static void *blocked_thread(void *argument)
{
    (void)argument;
    for (;;) {
        (void)sapote_sleep_until(sapote_monotonic_ns() +
            UINT64_C(1000000000));
    }
}

static _Noreturn void poison_state_and_fault(void)
{
    static const uint64_t pattern[2] = {
        UINT64_C(0xD15EA5E0D15EA5E0), UINT64_C(0x2EA15A1F2EA15A1F)
    };

    __asm__ volatile("movdqu %0, %%xmm15\n\tfldpi" : : "m" (pattern) :
        "xmm15", "memory");
    *(volatile uint64_t *)(uintptr_t)0U = UINT64_C(0xBADF00D);
    __builtin_unreachable();
}

int main(void)
{
    struct sapote_memory_map_response mapping = {0U, 0U, 0U, 0U};
    struct sapote_window_create_response window = {0U};
    pthread_t thread;
    long file;
    long directory;
    long timer;

    if (sapote_path_mkdir(SAPOTE_VOLUME_DATA, "LIVE") != 0) return 10;
    file = sapote_file_open(SAPOTE_VOLUME_DATA, "LIVE/OPEN.TXT",
        SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE | SAPOTE_OPEN_TRUNCATE);
    directory = sapote_directory_open(SAPOTE_VOLUME_DATA, "LIVE");
    timer = sapote_timer_create();
    if (file < 0 || directory < 0 || timer < 0 ||
        sapote_file_write((sapote_handle_t)file, "live", 4U) != 4 ||
        sapote_timer_set((sapote_handle_t)timer,
            sapote_monotonic_ns() + UINT64_C(1000000000)) != 0 ||
        sapote_memory_allocate(2U * SAPOTE_ABI_PAGE_SIZE,
            SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE, &mapping) != 0 ||
        sapote_window_create("Crash containment", 160U, 96U, &window) != 0 ||
        pthread_create(&thread, NULL, blocked_thread, NULL) != 0) {
        return 11;
    }
    poison_state_and_fault();
}
