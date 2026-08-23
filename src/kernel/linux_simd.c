/* SPDX-License-Identifier: GPL-3.0-only */
/* Explicit CPU control and clean-state foundation for Linux Ring 3 SIMD. */

#include <sapote/linux_simd.h>

#include <stdint.h>

#include <sapote/cpu.h>

#define CPUID_BASIC_ROOT UINT32_C(0)
#define CPUID_BASIC_FEATURES UINT32_C(1)
#define CPUID_FEATURE_FXSR (UINT32_C(1) << 24)
#define CPUID_FEATURE_SSE (UINT32_C(1) << 25)
#define CPUID_FEATURE_SSE2 (UINT32_C(1) << 26)
#define CR0_MONITOR_COPROCESSOR (UINT64_C(1) << 1)
#define CR0_EMULATION (UINT64_C(1) << 2)
#define CR0_TASK_SWITCHED (UINT64_C(1) << 3)
#define CR4_OS_FXSAVE_FXRSTOR (UINT64_C(1) << 9)
#define CR4_OS_UNMASKED_SIMD_EXCEPTIONS (UINT64_C(1) << 10)
#define LINUX_MXCSR_DEFAULT UINT32_C(0x1F80)
#define LINUX_X87_CONTROL_DEFAULT UINT16_C(0x037F)

static bool installed;

static bool features_valid(uint32_t features)
{
    const uint32_t required = CPUID_FEATURE_FXSR | CPUID_FEATURE_SSE |
        CPUID_FEATURE_SSE2;

    return (features & required) == required;
}

static bool controls_valid(uint64_t cr0, uint64_t cr4)
{
    return (cr0 & CR0_MONITOR_COPROCESSOR) != 0U &&
        (cr0 & (CR0_EMULATION | CR0_TASK_SWITCHED)) == 0U &&
        (cr4 & CR4_OS_FXSAVE_FXRSTOR) != 0U &&
        (cr4 & CR4_OS_UNMASKED_SIMD_EXCEPTIONS) != 0U;
}

static bool defaults_valid(void)
{
    return cpu_read_mxcsr() == LINUX_MXCSR_DEFAULT &&
        cpu_read_x87_control() == LINUX_X87_CONTROL_DEFAULT;
}

bool linux_simd_foundation_install(size_t *completed_tests)
{
    struct cpuid_result root;
    struct cpuid_result features;
    uint64_t original_cr0;
    uint64_t original_cr4;
    uint64_t cr0;
    uint64_t cr4;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (installed) {
        return false;
    }
    cpu_cpuid(CPUID_BASIC_ROOT, 0U, &root);
    if (root.eax < CPUID_BASIC_FEATURES) {
        return false;
    }
    cpu_cpuid(CPUID_BASIC_FEATURES, 0U, &features);
    original_cr0 = cpu_read_cr0();
    original_cr4 = cpu_read_cr4();
    cr0 = (original_cr0 | CR0_MONITOR_COPROCESSOR) &
        ~(CR0_EMULATION | CR0_TASK_SWITCHED);
    cr4 = original_cr4 | CR4_OS_FXSAVE_FXRSTOR |
        CR4_OS_UNMASKED_SIMD_EXCEPTIONS;
    if (!features_valid(features.edx) ||
        features_valid(features.edx & ~CPUID_FEATURE_FXSR) ||
        features_valid(features.edx & ~CPUID_FEATURE_SSE) ||
        features_valid(features.edx & ~CPUID_FEATURE_SSE2) ||
        !controls_valid(cr0, cr4) ||
        controls_valid(cr0 & ~CR0_MONITOR_COPROCESSOR, cr4) ||
        controls_valid(cr0 | CR0_EMULATION, cr4) ||
        controls_valid(cr0 | CR0_TASK_SWITCHED, cr4) ||
        controls_valid(cr0, cr4 & ~CR4_OS_FXSAVE_FXRSTOR) ||
        controls_valid(cr0, cr4 & ~CR4_OS_UNMASKED_SIMD_EXCEPTIONS)) {
        return false;
    }
    cpu_write_cr0(cr0);
    cpu_write_cr4(cr4);
    if (!controls_valid(cpu_read_cr0(), cpu_read_cr4())) {
        cpu_write_cr4(original_cr4);
        cpu_write_cr0(original_cr0);
        return false;
    }
    cpu_linux_simd_reset();
    if (!defaults_valid()) {
        cpu_write_cr4(original_cr4);
        cpu_write_cr0(original_cr0);
        return false;
    }
    installed = true;
    *completed_tests = LINUX_SIMD_FOUNDATION_CONTROLS;
    return true;
}

bool linux_simd_foundation_active(void)
{
    return installed && controls_valid(cpu_read_cr0(), cpu_read_cr4());
}

bool linux_simd_reset_user(void)
{
    if (!linux_simd_foundation_active()) {
        return false;
    }
    cpu_linux_simd_reset();
    return defaults_valid();
}
