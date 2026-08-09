/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/pic.h>
#include <zenith/pit.h>

/* Intel SDM Vol. 3, Local APIC and I/O APIC chapters. */
#define APIC_CPUID_FEATURE UINT32_C(1)
#define APIC_CPUID_EDX_APIC (UINT32_C(1) << 9U)
#define APIC_BASE_MSR UINT32_C(0x1B)
#define APIC_BASE_BSP (UINT64_C(1) << 8U)
#define APIC_BASE_X2APIC (UINT64_C(1) << 10U)
#define APIC_BASE_GLOBAL_ENABLE (UINT64_C(1) << 11U)
#define APIC_BASE_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

#define LOCAL_APIC_ID UINT32_C(0x020)
#define LOCAL_APIC_VERSION UINT32_C(0x030)
#define LOCAL_APIC_TASK_PRIORITY UINT32_C(0x080)
#define LOCAL_APIC_EOI UINT32_C(0x0B0)
#define LOCAL_APIC_SPURIOUS UINT32_C(0x0F0)
#define LOCAL_APIC_ERROR_STATUS UINT32_C(0x280)
#define LOCAL_APIC_LVT_TIMER UINT32_C(0x320)
#define LOCAL_APIC_LVT_ERROR UINT32_C(0x370)
#define LOCAL_APIC_TIMER_INITIAL_COUNT UINT32_C(0x380)
#define LOCAL_APIC_TIMER_CURRENT_COUNT UINT32_C(0x390)
#define LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION UINT32_C(0x3E0)
#define LOCAL_APIC_ID_SHIFT 24U
#define LOCAL_APIC_VERSION_MASK UINT32_C(0xFF)
#define LOCAL_APIC_MAX_LVT_SHIFT 16U
#define LOCAL_APIC_SOFTWARE_ENABLE (UINT32_C(1) << 8U)
#define LOCAL_APIC_LVT_DELIVERY_STATUS (UINT32_C(1) << 12U)
#define LOCAL_APIC_LVT_MASKED (UINT32_C(1) << 16U)
#define LOCAL_APIC_TIMER_PERIODIC (UINT32_C(1) << 17U)
#define LOCAL_APIC_TIMER_TSC_DEADLINE (UINT32_C(1) << 18U)
#define LOCAL_APIC_TIMER_MODE_MASK \
    (LOCAL_APIC_TIMER_PERIODIC | LOCAL_APIC_TIMER_TSC_DEADLINE)
#define LOCAL_APIC_TIMER_LVT_OWNED_MASK \
    (UINT32_C(0xFF) | LOCAL_APIC_LVT_MASKED | LOCAL_APIC_TIMER_MODE_MASK)
#define LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK UINT32_C(0x0B)
#define LOCAL_APIC_TIMER_DIVIDE_BY_16 UINT32_C(0x03)
#define LOCAL_APIC_REQUIRED_MAX_LVT 5U
#define LOCAL_APIC_INTEGRATED_VERSION UINT32_C(0x10)
#define LOCAL_APIC_ERROR_VECTOR UINT8_C(0xFE)

/* Intel 82093AA I/O APIC register interface and redirection table. */
#define IO_APIC_SELECTOR UINT32_C(0x00)
#define IO_APIC_WINDOW UINT32_C(0x10)
#define IO_APIC_ID UINT32_C(0x00)
#define IO_APIC_VERSION UINT32_C(0x01)
#define IO_APIC_REDIRECTION_BASE UINT32_C(0x10)
#define IO_APIC_ID_SHIFT 24U
#define IO_APIC_ID_MASK UINT32_C(0x0F)
#define IO_APIC_VERSION_MASK UINT32_C(0xFF)
#define IO_APIC_MAX_REDIRECTION_SHIFT 16U
#define IO_APIC_MAX_REDIRECTION_MASK UINT32_C(0xFF)
#define IO_APIC_REDIRECTION_POLARITY_LOW (UINT32_C(1) << 13U)
#define IO_APIC_REDIRECTION_TRIGGER_LEVEL (UINT32_C(1) << 15U)
#define IO_APIC_REDIRECTION_MASKED (UINT32_C(1) << 16U)
#define IO_APIC_REDIRECTION_DESTINATION_MASK UINT32_C(0xFF000000)
#define IO_APIC_REDIRECTION_LOW_WRITABLE_MASK UINT32_C(0x0001AFFF)
#define IO_APIC_REGISTER_SELECTOR_LIMIT UINT32_C(0xFF)
#define IO_APIC_MAX_REDIRECTION_ENTRIES \
    (((IO_APIC_REGISTER_SELECTOR_LIMIT - IO_APIC_REDIRECTION_BASE) / 2U) + 1U)

#define APIC_PAGE_MASK UINT64_C(0xFFF)
#define APIC_TIMER_REFERENCE_FREQUENCY_HZ UINT32_C(20)
#define APIC_TIMER_REFERENCE_TICKS UINT32_C(32)
#define APIC_TIMER_MAX_REFERENCE_TICKS UINT32_C(64)
#define APIC_TIMER_MIN_PERIODIC_FREQUENCY_HZ UINT32_C(10)
#define APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ UINT32_C(10000)
#define APIC_TIMER_MIN_DIVIDED_FREQUENCY_HZ UINT32_C(100000)
#define APIC_TIMER_MIN_INITIAL_COUNT UINT32_C(100)
#define APIC_TIMER_MAX_SAMPLE_SPREAD_PERCENT UINT32_C(5)
#define APIC_TIMER_MAX_FREQUENCY_ERROR_PERCENT UINT32_C(1)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

struct io_apic_probe {
    uint32_t id;
    uint32_t version;
    uint32_t redirection_entry_count;
};

struct apic_timer_sample {
    uint32_t elapsed_count;
    uint32_t pit_ticks;
};

static struct apic_configuration active_configuration;
static struct apic_local_timer_configuration active_timer_configuration;
static uint64_t active_local_apic_virtual_address;
static volatile uint64_t end_of_interrupt_count;
static volatile uint64_t spurious_interrupt_count;
static bool pit_route_masked;
static bool local_timer_active;
static bool initialized;

_Static_assert(APIC_TIMER_VECTOR >= INTERRUPT_EXCEPTION_COUNT,
               "APIC-routed PIT vector overlaps CPU exceptions");
_Static_assert(APIC_LOCAL_TIMER_VECTOR >= INTERRUPT_PIC_LIMIT,
               "Local APIC timer vector overlaps legacy PIC vectors");
_Static_assert(APIC_SPURIOUS_VECTOR != INTERRUPT_IST_TEST_VECTOR,
               "APIC spurious vector overlaps the IST probe");
_Static_assert(APIC_LOCAL_TIMER_VECTOR != APIC_SPURIOUS_VECTOR,
               "Local APIC timer vector overlaps the spurious vector");
_Static_assert(APIC_TIMER_CALIBRATION_SAMPLE_COUNT % 2U == 1U,
               "calibration requires an odd sample count");

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static void timer_configuration_reset(
    struct apic_local_timer_configuration *configuration
)
{
    bytes_zero(configuration, sizeof(*configuration));
}

static void timer_configuration_copy(
    struct apic_local_timer_configuration *destination,
    const struct apic_local_timer_configuration *source
)
{
    destination->divide_configuration = source->divide_configuration;
    destination->divisor = source->divisor;
    destination->calibrated_frequency_hz =
        source->calibrated_frequency_hz;
    destination->periodic_initial_count = source->periodic_initial_count;
    destination->requested_frequency_hz = source->requested_frequency_hz;
    destination->periodic_frequency_hz = source->periodic_frequency_hz;
    destination->minimum_sample_hz = source->minimum_sample_hz;
    destination->maximum_sample_hz = source->maximum_sample_hz;
    destination->period_nanoseconds = source->period_nanoseconds;
}

static enum apic_status timer_divisor_decode(
    uint32_t divide_configuration,
    uint32_t *divisor
)
{
    if (divisor == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    if ((divide_configuration &
         ~LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK) != 0U) {
        return APIC_STATUS_BAD_TIMER_DIVIDE;
    }

    switch (divide_configuration) {
    case UINT32_C(0x0B):
        *divisor = 1U;
        break;
    case UINT32_C(0x00):
        *divisor = 2U;
        break;
    case UINT32_C(0x01):
        *divisor = 4U;
        break;
    case UINT32_C(0x02):
        *divisor = 8U;
        break;
    case UINT32_C(0x03):
        *divisor = 16U;
        break;
    case UINT32_C(0x08):
        *divisor = 32U;
        break;
    case UINT32_C(0x09):
        *divisor = 64U;
        break;
    case UINT32_C(0x0A):
        *divisor = 128U;
        break;
    default:
        return APIC_STATUS_BAD_TIMER_DIVIDE;
    }

    return APIC_STATUS_OK;
}

static enum apic_status timer_sample_frequency(
    const struct apic_timer_sample *sample,
    uint32_t pit_divisor_value,
    uint32_t *frequency_hz
)
{
    uint64_t numerator;
    uint64_t denominator;
    uint64_t rounded_frequency;

    if (sample == NULL || frequency_hz == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    if (sample->elapsed_count == 0U || sample->pit_ticks == 0U ||
        sample->pit_ticks > APIC_TIMER_MAX_REFERENCE_TICKS ||
        pit_divisor_value == 0U || pit_divisor_value > UINT16_MAX) {
        return APIC_STATUS_BAD_TIMER_SAMPLE;
    }

    numerator = (uint64_t)sample->elapsed_count *
        PIT_INPUT_FREQUENCY_HZ;
    denominator = (uint64_t)pit_divisor_value * sample->pit_ticks;
    rounded_frequency = (numerator + denominator / 2U) / denominator;

    if (rounded_frequency < APIC_TIMER_MIN_DIVIDED_FREQUENCY_HZ ||
        rounded_frequency > UINT32_MAX) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    *frequency_hz = (uint32_t)rounded_frequency;
    return APIC_STATUS_OK;
}

static enum apic_status timer_configuration_build(
    const struct apic_timer_sample *samples,
    size_t sample_count,
    uint32_t pit_divisor_value,
    uint32_t divide_configuration,
    uint32_t periodic_frequency_hz,
    struct apic_local_timer_configuration *configuration
)
{
    uint32_t frequencies[APIC_TIMER_CALIBRATION_SAMPLE_COUNT];
    uint32_t divisor;
    uint32_t calibrated_frequency;
    uint32_t periodic_initial_count;
    uint32_t actual_periodic_frequency;
    uint64_t period_numerator;
    enum apic_status status;

    if (samples == NULL || configuration == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    timer_configuration_reset(configuration);

    if (sample_count != APIC_TIMER_CALIBRATION_SAMPLE_COUNT) {
        return APIC_STATUS_BAD_TIMER_SAMPLE;
    }

    if (periodic_frequency_hz < APIC_TIMER_MIN_PERIODIC_FREQUENCY_HZ ||
        periodic_frequency_hz > APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    status = timer_divisor_decode(divide_configuration, &divisor);

    if (status != APIC_STATUS_OK) {
        return status;
    }

    for (size_t index = 0U; index < sample_count; ++index) {
        status = timer_sample_frequency(
            &samples[index],
            pit_divisor_value,
            &frequencies[index]
        );

        if (status != APIC_STATUS_OK) {
            return status;
        }
    }

    for (size_t index = 1U; index < sample_count; ++index) {
        uint32_t value = frequencies[index];
        size_t position = index;

        while (position != 0U && frequencies[position - 1U] > value) {
            frequencies[position] = frequencies[position - 1U];
            --position;
        }

        frequencies[position] = value;
    }

    calibrated_frequency = frequencies[sample_count / 2U];

    for (size_t index = 0U; index < sample_count; ++index) {
        const uint32_t difference = frequencies[index] >
                calibrated_frequency
            ? frequencies[index] - calibrated_frequency
            : calibrated_frequency - frequencies[index];

        if ((uint64_t)difference * UINT32_C(100) >
            (uint64_t)calibrated_frequency *
                APIC_TIMER_MAX_SAMPLE_SPREAD_PERCENT) {
            return APIC_STATUS_BAD_TIMER_SAMPLE;
        }
    }

    periodic_initial_count = (uint32_t)(
        ((uint64_t)calibrated_frequency + periodic_frequency_hz / 2U) /
            periodic_frequency_hz
    );

    if (periodic_initial_count < APIC_TIMER_MIN_INITIAL_COUNT) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    actual_periodic_frequency = (uint32_t)(
        ((uint64_t)calibrated_frequency + periodic_initial_count / 2U) /
            periodic_initial_count
    );

    if (actual_periodic_frequency == 0U ||
        (uint64_t)(actual_periodic_frequency > periodic_frequency_hz
            ? actual_periodic_frequency - periodic_frequency_hz
            : periodic_frequency_hz - actual_periodic_frequency) *
                UINT32_C(100) >
            (uint64_t)periodic_frequency_hz *
                APIC_TIMER_MAX_FREQUENCY_ERROR_PERCENT) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    period_numerator = (uint64_t)periodic_initial_count *
        NANOSECONDS_PER_SECOND;
    configuration->divide_configuration = divide_configuration;
    configuration->divisor = divisor;
    configuration->calibrated_frequency_hz = calibrated_frequency;
    configuration->periodic_initial_count = periodic_initial_count;
    configuration->requested_frequency_hz = periodic_frequency_hz;
    configuration->periodic_frequency_hz = actual_periodic_frequency;
    configuration->minimum_sample_hz = frequencies[0];
    configuration->maximum_sample_hz = frequencies[sample_count - 1U];
    configuration->period_nanoseconds =
        (period_numerator + calibrated_frequency / 2U) /
            calibrated_frequency;

    if (configuration->period_nanoseconds == 0U) {
        timer_configuration_reset(configuration);
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    return APIC_STATUS_OK;
}

static bool page_aligned(uint64_t address)
{
    return address != 0U && (address & APIC_PAGE_MASK) == 0U;
}

static uint32_t mmio_read32(uint64_t base, uint32_t offset)
{
    const volatile uint32_t *register_address =
        (const volatile uint32_t *)(uintptr_t)(base + offset);

    /* Volatile is required because this access is an architected MMIO action. */
    return *register_address;
}

static void mmio_write32(uint64_t base, uint32_t offset, uint32_t value)
{
    volatile uint32_t *register_address =
        (volatile uint32_t *)(uintptr_t)(base + offset);

    /* Volatile is required because this access is an architected MMIO action. */
    *register_address = value;
}

static uint32_t io_apic_read(uint64_t base, uint32_t register_index)
{
    mmio_write32(base, IO_APIC_SELECTOR, register_index);
    return mmio_read32(base, IO_APIC_WINDOW);
}

static void io_apic_write(
    uint64_t base,
    uint32_t register_index,
    uint32_t value
)
{
    mmio_write32(base, IO_APIC_SELECTOR, register_index);
    mmio_write32(base, IO_APIC_WINDOW, value);
}

static uint32_t redirection_low_register(uint32_t input)
{
    return IO_APIC_REDIRECTION_BASE + input * 2U;
}

static uint32_t redirection_high_register(uint32_t input)
{
    return redirection_low_register(input) + 1U;
}

static void configuration_reset(struct apic_configuration *configuration)
{
    bytes_zero(configuration, sizeof(*configuration));
}

static void configuration_copy(
    struct apic_configuration *destination,
    const struct apic_configuration *source
)
{
    destination->local_apic_physical_address =
        source->local_apic_physical_address;
    destination->local_apic_id = source->local_apic_id;
    destination->local_apic_version = source->local_apic_version;
    destination->local_apic_max_lvt = source->local_apic_max_lvt;
    destination->io_apic_count = source->io_apic_count;
    destination->timer_global_interrupt = source->timer_global_interrupt;
    destination->timer_input = source->timer_input;
    destination->timer_io_apic_index = source->timer_io_apic_index;
    destination->timer_active_low = source->timer_active_low;
    destination->timer_level_triggered = source->timer_level_triggered;

    for (size_t index = 0U; index < ACPI_MAX_IO_APICS; ++index) {
        destination->io_apics[index].virtual_address =
            source->io_apics[index].virtual_address;
        destination->io_apics[index].physical_address =
            source->io_apics[index].physical_address;
        destination->io_apics[index].id = source->io_apics[index].id;
        destination->io_apics[index].version =
            source->io_apics[index].version;
        destination->io_apics[index].global_interrupt_base =
            source->io_apics[index].global_interrupt_base;
        destination->io_apics[index].redirection_entry_count =
            source->io_apics[index].redirection_entry_count;
    }
}

static bool configuration_equal(
    const struct apic_configuration *left,
    const struct apic_configuration *right
)
{
    if (left->local_apic_physical_address !=
            right->local_apic_physical_address ||
        left->local_apic_id != right->local_apic_id ||
        left->local_apic_version != right->local_apic_version ||
        left->local_apic_max_lvt != right->local_apic_max_lvt ||
        left->io_apic_count != right->io_apic_count ||
        left->timer_global_interrupt != right->timer_global_interrupt ||
        left->timer_input != right->timer_input ||
        left->timer_io_apic_index != right->timer_io_apic_index ||
        left->timer_active_low != right->timer_active_low ||
        left->timer_level_triggered != right->timer_level_triggered) {
        return false;
    }

    for (size_t index = 0U; index < left->io_apic_count; ++index) {
        const struct apic_io_controller *left_io = &left->io_apics[index];
        const struct apic_io_controller *right_io = &right->io_apics[index];

        if (left_io->virtual_address != right_io->virtual_address ||
            left_io->physical_address != right_io->physical_address ||
            left_io->id != right_io->id ||
            left_io->version != right_io->version ||
            left_io->global_interrupt_base !=
                right_io->global_interrupt_base ||
            left_io->redirection_entry_count !=
                right_io->redirection_entry_count) {
            return false;
        }
    }

    return true;
}

static enum apic_status decode_timer_override(
    const struct acpi_topology *topology,
    uint32_t *global_interrupt,
    bool *active_low,
    bool *level_triggered
)
{
    bool found = false;

    *global_interrupt = 0U;
    *active_low = false;
    *level_triggered = false;

    if (topology->interrupt_override_count >
        ACPI_MAX_INTERRUPT_OVERRIDES) {
        return APIC_STATUS_BAD_TIMER_OVERRIDE;
    }

    for (size_t index = 0U;
         index < topology->interrupt_override_count;
         ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];

        if (override->source != 0U) {
            continue;
        }

        if (found) {
            return APIC_STATUS_BAD_TIMER_OVERRIDE;
        }

        found = true;
        *global_interrupt = override->global_interrupt;

        switch (override->polarity) {
        case ACPI_POLARITY_CONFORMS:
        case ACPI_POLARITY_ACTIVE_HIGH:
            *active_low = false;
            break;
        case ACPI_POLARITY_ACTIVE_LOW:
            *active_low = true;
            break;
        default:
            return APIC_STATUS_BAD_TIMER_OVERRIDE;
        }

        switch (override->trigger) {
        case ACPI_TRIGGER_CONFORMS:
        case ACPI_TRIGGER_EDGE:
            *level_triggered = false;
            break;
        case ACPI_TRIGGER_LEVEL:
            *level_triggered = true;
            break;
        default:
            return APIC_STATUS_BAD_TIMER_OVERRIDE;
        }
    }

    return APIC_STATUS_OK;
}

static enum apic_status configuration_build(
    const struct acpi_topology *topology,
    const struct virtual_memory_layout *layout,
    uint32_t local_apic_id,
    uint32_t local_apic_version,
    uint32_t local_apic_max_lvt,
    const struct io_apic_probe *probes,
    struct apic_configuration *configuration
)
{
    uint64_t range_ends[ACPI_MAX_IO_APICS];
    uint32_t timer_gsi;
    bool timer_active_low;
    bool timer_level_triggered;
    bool local_processor_found = false;
    size_t timer_controller = ACPI_MAX_IO_APICS;
    enum apic_status status;

    if (topology == NULL || layout == NULL || probes == NULL ||
        configuration == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    configuration_reset(configuration);

    if ((local_apic_version & UINT32_C(0xF0)) !=
            LOCAL_APIC_INTEGRATED_VERSION ||
        local_apic_max_lvt < LOCAL_APIC_REQUIRED_MAX_LVT) {
        return APIC_STATUS_BAD_LOCAL_APIC_VERSION;
    }

    if (topology->processor_count == 0U ||
        topology->processor_count > ACPI_MAX_PROCESSORS) {
        return APIC_STATUS_LOCAL_APIC_ID_MISMATCH;
    }

    for (size_t index = 0U; index < topology->processor_count; ++index) {
        if (topology->processors[index].apic_id == local_apic_id) {
            local_processor_found = true;
            break;
        }
    }

    if (!local_processor_found) {
        return APIC_STATUS_LOCAL_APIC_ID_MISMATCH;
    }

    if (topology->io_apic_count == 0U ||
        topology->io_apic_count > ACPI_MAX_IO_APICS ||
        layout->io_apic_count != topology->io_apic_count) {
        return APIC_STATUS_BAD_IO_APIC_COUNT;
    }

    if (!page_aligned(topology->local_apic_address) ||
        !page_aligned(layout->local_apic_virtual_address)) {
        return APIC_STATUS_BAD_IO_APIC_ADDRESS;
    }

    configuration->local_apic_physical_address = topology->local_apic_address;
    configuration->local_apic_id = local_apic_id;
    configuration->local_apic_version = local_apic_version;
    configuration->local_apic_max_lvt = local_apic_max_lvt;
    configuration->io_apic_count = topology->io_apic_count;

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *topology_io = &topology->io_apics[index];
        struct apic_io_controller *controller =
            &configuration->io_apics[index];
        const uint64_t range_end =
            (uint64_t)topology_io->global_interrupt_base +
            probes[index].redirection_entry_count;

        if (!page_aligned(topology_io->physical_address) ||
            !page_aligned(layout->io_apic_virtual_addresses[index])) {
            return APIC_STATUS_BAD_IO_APIC_ADDRESS;
        }

        if (topology_io->id > IO_APIC_ID_MASK ||
            probes[index].id != topology_io->id) {
            return APIC_STATUS_BAD_IO_APIC_ID;
        }

        if (probes[index].version == 0U ||
            probes[index].redirection_entry_count == 0U ||
            probes[index].redirection_entry_count >
                IO_APIC_MAX_REDIRECTION_ENTRIES) {
            return APIC_STATUS_BAD_IO_APIC_VERSION;
        }

        if (range_end > (uint64_t)UINT32_MAX + 1U) {
            return APIC_STATUS_BAD_GSI_RANGE;
        }

        range_ends[index] = range_end;
        controller->virtual_address =
            layout->io_apic_virtual_addresses[index];
        controller->physical_address = topology_io->physical_address;
        controller->id = probes[index].id;
        controller->version = probes[index].version;
        controller->global_interrupt_base =
            topology_io->global_interrupt_base;
        controller->redirection_entry_count =
            probes[index].redirection_entry_count;
    }

    for (size_t left = 0U; left < topology->io_apic_count; ++left) {
        const uint64_t left_start =
            topology->io_apics[left].global_interrupt_base;

        for (size_t right = left + 1U;
             right < topology->io_apic_count;
             ++right) {
            const uint64_t right_start =
                topology->io_apics[right].global_interrupt_base;

            if (left_start < range_ends[right] &&
                right_start < range_ends[left]) {
                return APIC_STATUS_OVERLAPPING_GSI_RANGE;
            }
        }
    }

    status = decode_timer_override(
        topology,
        &timer_gsi,
        &timer_active_low,
        &timer_level_triggered
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        const uint64_t start = topology->io_apics[index].global_interrupt_base;

        if ((uint64_t)timer_gsi >= start &&
            (uint64_t)timer_gsi < range_ends[index]) {
            if (timer_controller != ACPI_MAX_IO_APICS) {
                return APIC_STATUS_OVERLAPPING_GSI_RANGE;
            }

            timer_controller = index;
        }
    }

    if (timer_controller == ACPI_MAX_IO_APICS) {
        return APIC_STATUS_TIMER_GSI_UNROUTABLE;
    }

    configuration->timer_global_interrupt = timer_gsi;
    configuration->timer_io_apic_index = timer_controller;
    configuration->timer_input = timer_gsi -
        topology->io_apics[timer_controller].global_interrupt_base;
    configuration->timer_active_low = timer_active_low;
    configuration->timer_level_triggered = timer_level_triggered;
    return APIC_STATUS_OK;
}

static bool device_mapping_matches(uint64_t virtual_address, uint64_t physical)
{
    struct virtual_memory_mapping mapping;

    return virtual_memory_query(virtual_address, &mapping) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        mapping.physical_address == physical &&
        mapping.permissions ==
            (VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE);
}

static enum apic_status hardware_probe(
    const struct acpi_topology *topology,
    const struct virtual_memory_layout *layout,
    struct io_apic_probe *probes,
    uint32_t *local_apic_id,
    uint32_t *local_apic_version,
    uint32_t *local_apic_max_lvt
)
{
    struct cpu_cpuid_result cpuid;
    struct cpu_cpuid_result highest;
    uint64_t apic_base;
    uint32_t local_version_register;

    if (topology == NULL || layout == NULL || probes == NULL ||
        local_apic_id == NULL || local_apic_version == NULL ||
        local_apic_max_lvt == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    if (topology->io_apic_count == 0U ||
        topology->io_apic_count > ACPI_MAX_IO_APICS ||
        layout->io_apic_count != topology->io_apic_count) {
        return APIC_STATUS_BAD_IO_APIC_COUNT;
    }

    if (!virtual_memory_ready() ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK) {
        return APIC_STATUS_VIRTUAL_MEMORY_FAILURE;
    }

    if (!device_mapping_matches(
            layout->local_apic_virtual_address,
            topology->local_apic_address
        )) {
        return APIC_STATUS_VIRTUAL_MEMORY_FAILURE;
    }

    cpu_cpuid(0U, 0U, &highest);

    if (highest.eax < APIC_CPUID_FEATURE) {
        return APIC_STATUS_UNSUPPORTED_CPU;
    }

    cpu_cpuid(APIC_CPUID_FEATURE, 0U, &cpuid);

    if ((cpuid.edx & APIC_CPUID_EDX_APIC) == 0U) {
        return APIC_STATUS_UNSUPPORTED_CPU;
    }

    apic_base = cpu_read_msr(APIC_BASE_MSR);

    if ((apic_base & APIC_BASE_X2APIC) != 0U) {
        return APIC_STATUS_X2APIC_ACTIVE;
    }

    if ((apic_base & APIC_BASE_GLOBAL_ENABLE) == 0U) {
        return APIC_STATUS_LOCAL_APIC_DISABLED;
    }

    if ((apic_base & APIC_BASE_BSP) == 0U) {
        return APIC_STATUS_NOT_BOOTSTRAP_PROCESSOR;
    }

    if ((apic_base & APIC_BASE_ADDRESS_MASK) !=
        topology->local_apic_address) {
        return APIC_STATUS_LOCAL_APIC_BASE_MISMATCH;
    }

    *local_apic_id = mmio_read32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_ID
    ) >> LOCAL_APIC_ID_SHIFT;
    local_version_register = mmio_read32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_VERSION
    );
    *local_apic_version =
        local_version_register & LOCAL_APIC_VERSION_MASK;
    *local_apic_max_lvt =
        (local_version_register >> LOCAL_APIC_MAX_LVT_SHIFT) & UINT32_C(0xFF);

    if (*local_apic_id != (cpuid.ebx >> 24U)) {
        return APIC_STATUS_LOCAL_APIC_ID_MISMATCH;
    }

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        uint32_t version_register;

        if (!device_mapping_matches(
                layout->io_apic_virtual_addresses[index],
                topology->io_apics[index].physical_address
            )) {
            return APIC_STATUS_VIRTUAL_MEMORY_FAILURE;
        }

        probes[index].id =
            (io_apic_read(
                layout->io_apic_virtual_addresses[index],
                IO_APIC_ID
            ) >> IO_APIC_ID_SHIFT) & IO_APIC_ID_MASK;
        version_register = io_apic_read(
            layout->io_apic_virtual_addresses[index],
            IO_APIC_VERSION
        );
        probes[index].version = version_register & IO_APIC_VERSION_MASK;
        probes[index].redirection_entry_count =
            ((version_register >> IO_APIC_MAX_REDIRECTION_SHIFT) &
                IO_APIC_MAX_REDIRECTION_MASK) + 1U;
    }

    return APIC_STATUS_OK;
}

static uint32_t expected_redirection_low(
    const struct apic_configuration *configuration,
    bool masked
)
{
    uint32_t value = APIC_TIMER_VECTOR;

    if (configuration->timer_active_low) {
        value |= IO_APIC_REDIRECTION_POLARITY_LOW;
    }

    if (configuration->timer_level_triggered) {
        value |= IO_APIC_REDIRECTION_TRIGGER_LEVEL;
    }

    if (masked) {
        value |= IO_APIC_REDIRECTION_MASKED;
    }

    return value;
}

static uint32_t expected_redirection_high(
    const struct apic_configuration *configuration
)
{
    return configuration->local_apic_id << IO_APIC_ID_SHIFT;
}

static bool route_register_matches(
    const struct apic_configuration *configuration,
    bool masked
)
{
    const struct apic_io_controller *controller =
        &configuration->io_apics[configuration->timer_io_apic_index];
    const uint32_t low = io_apic_read(
        controller->virtual_address,
        redirection_low_register(configuration->timer_input)
    );
    const uint32_t high = io_apic_read(
        controller->virtual_address,
        redirection_high_register(configuration->timer_input)
    );

    return (low & IO_APIC_REDIRECTION_LOW_WRITABLE_MASK) ==
            expected_redirection_low(configuration, masked) &&
        (high & IO_APIC_REDIRECTION_DESTINATION_MASK) ==
            expected_redirection_high(configuration);
}

static bool all_other_routes_masked(
    const struct apic_configuration *configuration
)
{
    for (size_t controller_index = 0U;
         controller_index < configuration->io_apic_count;
         ++controller_index) {
        const struct apic_io_controller *controller =
            &configuration->io_apics[controller_index];

        for (uint32_t input = 0U;
             input < controller->redirection_entry_count;
             ++input) {
            if (controller_index == configuration->timer_io_apic_index &&
                input == configuration->timer_input) {
                continue;
            }

            if ((io_apic_read(
                    controller->virtual_address,
                    redirection_low_register(input)
                ) & IO_APIC_REDIRECTION_MASKED) == 0U) {
                return false;
            }
        }
    }

    return true;
}

static void mask_all_routes(const struct apic_configuration *configuration)
{
    for (size_t controller_index = 0U;
         controller_index < configuration->io_apic_count;
         ++controller_index) {
        const struct apic_io_controller *controller =
            &configuration->io_apics[controller_index];

        for (uint32_t input = 0U;
             input < controller->redirection_entry_count;
             ++input) {
            const uint32_t register_index = redirection_low_register(input);
            const uint32_t value = io_apic_read(
                controller->virtual_address,
                register_index
            );

            io_apic_write(
                controller->virtual_address,
                register_index,
                value | IO_APIC_REDIRECTION_MASKED
            );
        }
    }
}

static void program_timer_route(
    const struct apic_configuration *configuration,
    bool masked
)
{
    const struct apic_io_controller *controller =
        &configuration->io_apics[configuration->timer_io_apic_index];

    io_apic_write(
        controller->virtual_address,
        redirection_high_register(configuration->timer_input),
        expected_redirection_high(configuration)
    );
    io_apic_write(
        controller->virtual_address,
        redirection_low_register(configuration->timer_input),
        expected_redirection_low(configuration, masked)
    );
}

static void spurious_handler(struct interrupt_frame *frame, void *context)
{
    (void)context;

    if (frame != NULL && frame->vector == APIC_SPURIOUS_VECTOR) {
        ++spurious_interrupt_count;
    }
}

static bool local_timer_registers_match(void)
{
    const uint32_t timer_lvt = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_LVT_TIMER
    );
    const uint32_t divide_configuration = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION
    );
    const uint32_t initial_count = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_INITIAL_COUNT
    );
    const uint32_t current_count = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_CURRENT_COUNT
    );
    const uint32_t expected_lvt = local_timer_active
        ? LOCAL_APIC_TIMER_PERIODIC | APIC_LOCAL_TIMER_VECTOR
        : LOCAL_APIC_LVT_MASKED | APIC_LOCAL_TIMER_VECTOR;
    const uint32_t expected_initial_count = local_timer_active
        ? active_timer_configuration.periodic_initial_count
        : 0U;
    const uint32_t expected_divide_configuration = local_timer_active
        ? active_timer_configuration.divide_configuration
        : LOCAL_APIC_TIMER_DIVIDE_BY_16;

    if ((timer_lvt & LOCAL_APIC_TIMER_LVT_OWNED_MASK) != expected_lvt ||
        (timer_lvt & ~(LOCAL_APIC_TIMER_LVT_OWNED_MASK |
            LOCAL_APIC_LVT_DELIVERY_STATUS)) != 0U ||
        (divide_configuration & ~LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK) !=
            0U ||
        (divide_configuration & LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK) !=
            expected_divide_configuration ||
        initial_count != expected_initial_count ||
        current_count > expected_initial_count) {
        return false;
    }

    return local_timer_active || current_count == 0U;
}

static bool active_registers_match(void)
{
    const uint32_t task_priority = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TASK_PRIORITY
    );
    const uint32_t spurious = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_SPURIOUS
    );
    const uint32_t error_lvt = mmio_read32(
        active_local_apic_virtual_address,
        LOCAL_APIC_LVT_ERROR
    );

    return (task_priority & UINT32_C(0xFF)) == 0U &&
        (spurious & (LOCAL_APIC_SOFTWARE_ENABLE | UINT32_C(0xFF))) ==
            (LOCAL_APIC_SOFTWARE_ENABLE | APIC_SPURIOUS_VECTOR) &&
        local_timer_registers_match() &&
        (error_lvt & LOCAL_APIC_LVT_MASKED) != 0U &&
        pic_all_masked() &&
        route_register_matches(&active_configuration, pit_route_masked) &&
        all_other_routes_masked(&active_configuration);
}

static void fixture_prepare(
    struct acpi_topology *topology,
    struct virtual_memory_layout *layout,
    struct io_apic_probe *probes
)
{
    bytes_zero(topology, sizeof(*topology));
    bytes_zero(layout, sizeof(*layout));
    bytes_zero(probes, sizeof(struct io_apic_probe) * ACPI_MAX_IO_APICS);

    topology->local_apic_address = UINT64_C(0xFEE00000);
    topology->processor_count = 1U;
    topology->processors[0].kind = ACPI_PROCESSOR_LOCAL_APIC;
    topology->processors[0].apic_id = 2U;
    topology->processors[0].flags = ACPI_PROCESSOR_ENABLED;
    topology->io_apic_count = 2U;
    topology->io_apics[0].id = 1U;
    topology->io_apics[0].physical_address = UINT32_C(0xFEC00000);
    topology->io_apics[0].global_interrupt_base = 0U;
    topology->io_apics[1].id = 2U;
    topology->io_apics[1].physical_address = UINT32_C(0xFEC01000);
    topology->io_apics[1].global_interrupt_base = 24U;
    topology->interrupt_override_count = 1U;
    topology->interrupt_overrides[0].source = 0U;
    topology->interrupt_overrides[0].global_interrupt = 2U;
    topology->interrupt_overrides[0].polarity = ACPI_POLARITY_ACTIVE_LOW;
    topology->interrupt_overrides[0].trigger = ACPI_TRIGGER_LEVEL;

    layout->local_apic_virtual_address =
        VIRTUAL_MEMORY_DEVICE_WINDOW_BASE;
    layout->io_apic_count = 2U;
    layout->io_apic_virtual_addresses[0] =
        VIRTUAL_MEMORY_DEVICE_WINDOW_BASE + UINT64_C(0x1000);
    layout->io_apic_virtual_addresses[1] =
        VIRTUAL_MEMORY_DEVICE_WINDOW_BASE + UINT64_C(0x2000);

    probes[0].id = 1U;
    probes[0].version = UINT32_C(0x20);
    probes[0].redirection_entry_count = 24U;
    probes[1].id = 2U;
    probes[1].version = UINT32_C(0x20);
    probes[1].redirection_entry_count = 24U;
}

bool apic_self_test(void)
{
    struct acpi_topology topology;
    struct virtual_memory_layout layout;
    struct io_apic_probe probes[ACPI_MAX_IO_APICS];
    struct apic_configuration configuration;

    if (initialized) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);

    if (configuration_build(
            &topology,
            &layout,
            2U,
            UINT32_C(0x14),
            6U,
            probes,
            &configuration
        ) != APIC_STATUS_OK ||
        configuration.timer_global_interrupt != 2U ||
        configuration.timer_io_apic_index != 0U ||
        configuration.timer_input != 2U ||
        !configuration.timer_active_low ||
        !configuration.timer_level_triggered ||
        configuration.io_apic_count != 2U) {
        return false;
    }

    if (configuration_build(NULL, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_NULL_ARGUMENT ||
        configuration_build(&topology, &layout, 2U, UINT32_C(0x01), 6U,
            probes, &configuration) != APIC_STATUS_BAD_LOCAL_APIC_VERSION) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.processors[0].apic_id = 3U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_LOCAL_APIC_ID_MISMATCH) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    layout.io_apic_count = 1U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_IO_APIC_COUNT) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    probes[0].id = 3U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_IO_APIC_ID) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    probes[0].redirection_entry_count = 0U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_IO_APIC_VERSION) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    probes[0].redirection_entry_count =
        IO_APIC_MAX_REDIRECTION_ENTRIES + 1U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_IO_APIC_VERSION) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.io_apics[0].global_interrupt_base = UINT32_MAX;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_GSI_RANGE) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.io_apics[1].global_interrupt_base = 23U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_OVERLAPPING_GSI_RANGE) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.io_apics[0].global_interrupt_base = 4U;
    topology.io_apics[1].global_interrupt_base = 28U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_TIMER_GSI_UNROUTABLE) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.interrupt_override_count = 2U;
    topology.interrupt_overrides[1].source = 0U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_TIMER_OVERRIDE) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.interrupt_overrides[0].polarity =
        (enum acpi_interrupt_polarity)2;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_BAD_TIMER_OVERRIDE) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    topology.interrupt_override_count = 0U;

    if (configuration_build(&topology, &layout, 2U, UINT32_C(0x14), 6U,
            probes, &configuration) != APIC_STATUS_OK ||
        configuration.timer_global_interrupt != 0U ||
        configuration.timer_input != 0U ||
        configuration.timer_active_low ||
        configuration.timer_level_triggered) {
        return false;
    }

    fixture_prepare(&topology, &layout, probes);
    layout.io_apic_virtual_addresses[0] += 1U;
    return configuration_build(
        &topology,
        &layout,
        2U,
        UINT32_C(0x14),
        6U,
        probes,
        &configuration
    ) == APIC_STATUS_BAD_IO_APIC_ADDRESS;
}

bool apic_local_timer_self_test(void)
{
    static const uint32_t divide_configurations[] = {
        UINT32_C(0x0B), UINT32_C(0x00), UINT32_C(0x01), UINT32_C(0x02),
        UINT32_C(0x03), UINT32_C(0x08), UINT32_C(0x09), UINT32_C(0x0A)
    };
    static const uint32_t expected_divisors[] = {
        1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U
    };
    struct apic_timer_sample samples[APIC_TIMER_CALIBRATION_SAMPLE_COUNT];
    struct apic_local_timer_configuration configuration;
    uint32_t divisor;

    if (initialized || local_timer_active) {
        return false;
    }

    for (size_t index = 0U;
         index < sizeof(divide_configurations) /
            sizeof(divide_configurations[0]);
         ++index) {
        if (timer_divisor_decode(divide_configurations[index], &divisor) !=
                APIC_STATUS_OK ||
            divisor != expected_divisors[index]) {
            return false;
        }
    }

    if (timer_divisor_decode(UINT32_C(0x04), &divisor) !=
            APIC_STATUS_BAD_TIMER_DIVIDE ||
        timer_divisor_decode(UINT32_MAX, &divisor) !=
            APIC_STATUS_BAD_TIMER_DIVIDE ||
        timer_divisor_decode(LOCAL_APIC_TIMER_DIVIDE_BY_16, NULL) !=
            APIC_STATUS_NULL_ARGUMENT) {
        return false;
    }

    for (size_t index = 0U;
         index < APIC_TIMER_CALIBRATION_SAMPLE_COUNT;
         ++index) {
        samples[index].elapsed_count = UINT32_C(2000000) +
            (uint32_t)index * UINT32_C(2000);
        samples[index].pit_ticks = APIC_TIMER_REFERENCE_TICKS;
    }

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_OK ||
        configuration.divisor != 16U ||
        configuration.requested_frequency_hz != UINT32_C(1000) ||
        configuration.periodic_frequency_hz != UINT32_C(1000) ||
        configuration.periodic_initial_count <
            APIC_TIMER_MIN_INITIAL_COUNT ||
        configuration.minimum_sample_hz >
            configuration.calibrated_frequency_hz ||
        configuration.maximum_sample_hz <
            configuration.calibrated_frequency_hz ||
        configuration.period_nanoseconds < UINT64_C(999000) ||
        configuration.period_nanoseconds > UINT64_C(1001000)) {
        return false;
    }

    if (timer_configuration_build(
            NULL,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_NULL_ARGUMENT ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            NULL
        ) != APIC_STATUS_NULL_ARGUMENT ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT - 1U,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            UINT32_C(0x04),
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_DIVIDE ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            APIC_TIMER_MIN_PERIODIC_FREQUENCY_HZ - 1U,
            &configuration
        ) != APIC_STATUS_BAD_TIMER_FREQUENCY ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ + 1U,
            &configuration
        ) != APIC_STATUS_BAD_TIMER_FREQUENCY ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            0U,
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            (uint32_t)UINT16_MAX + 1U,
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE) {
        return false;
    }

    samples[0].elapsed_count = 0U;

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE) {
        return false;
    }

    samples[0].elapsed_count = UINT32_C(2000000);
    samples[0].pit_ticks = 0U;

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE) {
        return false;
    }

    samples[0].pit_ticks = APIC_TIMER_MAX_REFERENCE_TICKS + 1U;

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE) {
        return false;
    }

    samples[0].pit_ticks = APIC_TIMER_REFERENCE_TICKS;
    samples[0].elapsed_count = UINT32_C(4000000);

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) != APIC_STATUS_BAD_TIMER_SAMPLE) {
        return false;
    }

    for (size_t index = 0U;
         index < APIC_TIMER_CALIBRATION_SAMPLE_COUNT;
         ++index) {
        samples[index].elapsed_count = UINT32_MAX;
        samples[index].pit_ticks = APIC_TIMER_MAX_REFERENCE_TICKS;
    }

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT16_MAX,
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            APIC_TIMER_MIN_PERIODIC_FREQUENCY_HZ,
            &configuration
        ) != APIC_STATUS_OK ||
        configuration.periodic_initial_count <
            APIC_TIMER_MIN_INITIAL_COUNT ||
        timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT16_MAX,
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ,
            &configuration
        ) != APIC_STATUS_OK) {
        return false;
    }

    for (size_t index = 0U;
         index < APIC_TIMER_CALIBRATION_SAMPLE_COUNT;
         ++index) {
        samples[index].elapsed_count = UINT32_C(3200);
        samples[index].pit_ticks = APIC_TIMER_REFERENCE_TICKS;
    }

    if (timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            UINT32_C(1193),
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ,
            &configuration
        ) != APIC_STATUS_BAD_TIMER_FREQUENCY) {
        return false;
    }

    samples[0].elapsed_count = UINT32_MAX;
    samples[0].pit_ticks = 1U;
    return timer_configuration_build(
            samples,
            APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
            1U,
            LOCAL_APIC_TIMER_DIVIDE_BY_16,
            UINT32_C(1000),
            &configuration
        ) == APIC_STATUS_BAD_TIMER_FREQUENCY;
}

enum apic_status apic_initialize(
    const struct acpi_topology *topology,
    const struct virtual_memory_layout *layout,
    struct apic_configuration *configuration
)
{
    struct io_apic_probe probes[ACPI_MAX_IO_APICS];
    struct apic_configuration candidate;
    uint32_t local_apic_id;
    uint32_t local_apic_version;
    uint32_t local_apic_max_lvt;
    uint32_t spurious;
    enum interrupt_status interrupt_status;
    enum apic_status status;

    if (topology == NULL || layout == NULL || configuration == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    configuration_reset(configuration);

    if (initialized) {
        return APIC_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (!interrupts_ready()) {
        return APIC_STATUS_INTERRUPT_FAILURE;
    }

    bytes_zero(probes, sizeof(probes));
    status = hardware_probe(
        topology,
        layout,
        probes,
        &local_apic_id,
        &local_apic_version,
        &local_apic_max_lvt
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    status = configuration_build(
        topology,
        layout,
        local_apic_id,
        local_apic_version,
        local_apic_max_lvt,
        probes,
        &candidate
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    pic_mask_all_early();

    if (!pic_all_masked()) {
        return APIC_STATUS_PIC_NOT_MASKED;
    }

    mask_all_routes(&candidate);

    if (!all_other_routes_masked(&candidate)) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    program_timer_route(&candidate, true);

    if (!route_register_matches(&candidate, true)) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    spurious_interrupt_count = 0U;
    interrupt_status = interrupt_register_handler(
        APIC_SPURIOUS_VECTOR,
        spurious_handler,
        NULL
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return APIC_STATUS_INTERRUPT_FAILURE;
    }

    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_TASK_PRIORITY,
        0U
    );
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_LVT_TIMER,
        LOCAL_APIC_LVT_MASKED | APIC_LOCAL_TIMER_VECTOR
    );
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION,
        LOCAL_APIC_TIMER_DIVIDE_BY_16
    );
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_TIMER_INITIAL_COUNT,
        0U
    );
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_LVT_ERROR,
        LOCAL_APIC_LVT_MASKED | LOCAL_APIC_ERROR_VECTOR
    );
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_ERROR_STATUS,
        0U
    );
    (void)mmio_read32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_ERROR_STATUS
    );

    spurious = mmio_read32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_SPURIOUS
    );
    spurious &= ~UINT32_C(0xFF);
    spurious |= LOCAL_APIC_SOFTWARE_ENABLE | APIC_SPURIOUS_VECTOR;
    mmio_write32(
        layout->local_apic_virtual_address,
        LOCAL_APIC_SPURIOUS,
        spurious
    );

    active_local_apic_virtual_address = layout->local_apic_virtual_address;
    configuration_copy(&active_configuration, &candidate);
    pit_route_masked = true;
    local_timer_active = false;
    timer_configuration_reset(&active_timer_configuration);
    end_of_interrupt_count = 0U;
    initialized = true;

    if (apic_validate() != APIC_STATUS_OK) {
        initialized = false;
        local_timer_active = false;
        active_local_apic_virtual_address = 0U;
        configuration_reset(&active_configuration);
        timer_configuration_reset(&active_timer_configuration);
        (void)interrupt_unregister_handler(APIC_SPURIOUS_VECTOR);
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    configuration_copy(configuration, &candidate);
    return APIC_STATUS_OK;
}

enum apic_status apic_validate(void)
{
    struct io_apic_probe probes[ACPI_MAX_IO_APICS];
    struct apic_configuration observed;
    struct acpi_topology topology;
    struct virtual_memory_layout layout;
    uint32_t local_apic_id;
    uint32_t local_apic_version;
    uint32_t local_apic_max_lvt;
    enum apic_status status;

    if (!initialized) {
        return APIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    bytes_zero(&topology, sizeof(topology));
    bytes_zero(&layout, sizeof(layout));
    bytes_zero(probes, sizeof(probes));
    topology.local_apic_address =
        active_configuration.local_apic_physical_address;
    topology.processor_count = 1U;
    topology.processors[0].apic_id = active_configuration.local_apic_id;
    topology.processors[0].flags = ACPI_PROCESSOR_ENABLED;
    topology.io_apic_count = active_configuration.io_apic_count;
    topology.interrupt_override_count = 1U;
    topology.interrupt_overrides[0].source = 0U;
    topology.interrupt_overrides[0].global_interrupt =
        active_configuration.timer_global_interrupt;
    topology.interrupt_overrides[0].polarity =
        active_configuration.timer_active_low ?
            ACPI_POLARITY_ACTIVE_LOW : ACPI_POLARITY_ACTIVE_HIGH;
    topology.interrupt_overrides[0].trigger =
        active_configuration.timer_level_triggered ?
            ACPI_TRIGGER_LEVEL : ACPI_TRIGGER_EDGE;
    layout.local_apic_virtual_address = active_local_apic_virtual_address;
    layout.io_apic_count = active_configuration.io_apic_count;

    for (size_t index = 0U;
         index < active_configuration.io_apic_count;
         ++index) {
        topology.io_apics[index].id = active_configuration.io_apics[index].id;
        topology.io_apics[index].physical_address = (uint32_t)
            active_configuration.io_apics[index].physical_address;
        topology.io_apics[index].global_interrupt_base =
            active_configuration.io_apics[index].global_interrupt_base;
        layout.io_apic_virtual_addresses[index] =
            active_configuration.io_apics[index].virtual_address;
    }

    status = hardware_probe(
        &topology,
        &layout,
        probes,
        &local_apic_id,
        &local_apic_version,
        &local_apic_max_lvt
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    status = configuration_build(
        &topology,
        &layout,
        local_apic_id,
        local_apic_version,
        local_apic_max_lvt,
        probes,
        &observed
    );

    if (status != APIC_STATUS_OK ||
        !configuration_equal(&active_configuration, &observed) ||
        !active_registers_match()) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    return APIC_STATUS_OK;
}

enum apic_status apic_timer_set_mask(bool masked)
{
    if (!initialized) {
        return APIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    program_timer_route(&active_configuration, masked);

    if (!route_register_matches(&active_configuration, masked)) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    pit_route_masked = masked;
    return APIC_STATUS_OK;
}

bool apic_timer_is_masked(void)
{
    return !initialized || pit_route_masked;
}

static void local_timer_reset_hardware(void)
{
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_LVT_TIMER,
        LOCAL_APIC_LVT_MASKED | APIC_LOCAL_TIMER_VECTOR
    );
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_INITIAL_COUNT,
        0U
    );
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION,
        LOCAL_APIC_TIMER_DIVIDE_BY_16
    );
}

static enum apic_status calibration_collect(
    struct apic_timer_sample samples[APIC_TIMER_CALIBRATION_SAMPLE_COUNT],
    uint32_t *pit_divisor_value
)
{
    enum apic_status status = APIC_STATUS_OK;
    enum pit_status pit_status;

    pit_status = pit_start(APIC_TIMER_REFERENCE_FREQUENCY_HZ);

    if (pit_status != PIT_STATUS_OK) {
        return APIC_STATUS_PIT_FAILURE;
    }

    *pit_divisor_value = pit_divisor();

    if (*pit_divisor_value == 0U || pit_frequency() == 0U) {
        status = APIC_STATUS_PIT_FAILURE;
    }

    for (size_t index = 0U;
         status == APIC_STATUS_OK &&
            index < APIC_TIMER_CALIBRATION_SAMPLE_COUNT;
         ++index) {
        uint64_t pit_start_tick;
        uint64_t pit_end_tick;
        uint64_t elapsed_pit_ticks;
        uint32_t timer_lvt;
        uint32_t divide_configuration;
        uint32_t initial_count;
        uint32_t current_count;

        pit_status = pit_wait_for_ticks(1U);

        if (pit_status != PIT_STATUS_OK) {
            status = pit_status == PIT_STATUS_TIMEOUT
                ? APIC_STATUS_TIMER_TIMEOUT
                : APIC_STATUS_PIT_FAILURE;
            break;
        }

        mmio_write32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION,
            LOCAL_APIC_TIMER_DIVIDE_BY_16
        );
        mmio_write32(
            active_local_apic_virtual_address,
            LOCAL_APIC_LVT_TIMER,
            LOCAL_APIC_LVT_MASKED | APIC_LOCAL_TIMER_VECTOR
        );
        mmio_write32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_INITIAL_COUNT,
            UINT32_MAX
        );
        timer_lvt = mmio_read32(
            active_local_apic_virtual_address,
            LOCAL_APIC_LVT_TIMER
        );
        divide_configuration = mmio_read32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION
        );
        initial_count = mmio_read32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_INITIAL_COUNT
        );

        if ((timer_lvt & LOCAL_APIC_TIMER_LVT_OWNED_MASK) !=
                (LOCAL_APIC_LVT_MASKED | APIC_LOCAL_TIMER_VECTOR) ||
            (timer_lvt & ~(LOCAL_APIC_TIMER_LVT_OWNED_MASK |
                LOCAL_APIC_LVT_DELIVERY_STATUS)) != 0U ||
            (divide_configuration &
                ~LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK) != 0U ||
            (divide_configuration &
                LOCAL_APIC_TIMER_DIVIDE_WRITABLE_MASK) !=
                LOCAL_APIC_TIMER_DIVIDE_BY_16 ||
            initial_count != UINT32_MAX) {
            status = APIC_STATUS_REGISTER_MISMATCH;
            break;
        }

        pit_start_tick = pit_ticks();
        pit_status = pit_wait_for_ticks(APIC_TIMER_REFERENCE_TICKS);
        pit_end_tick = pit_ticks();
        current_count = mmio_read32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_CURRENT_COUNT
        );
        mmio_write32(
            active_local_apic_virtual_address,
            LOCAL_APIC_TIMER_INITIAL_COUNT,
            0U
        );

        if (pit_status != PIT_STATUS_OK) {
            status = pit_status == PIT_STATUS_TIMEOUT
                ? APIC_STATUS_TIMER_TIMEOUT
                : APIC_STATUS_PIT_FAILURE;
            break;
        }

        if (pit_end_tick <= pit_start_tick || current_count == 0U) {
            status = APIC_STATUS_BAD_TIMER_SAMPLE;
            break;
        }

        elapsed_pit_ticks = pit_end_tick - pit_start_tick;

        if (elapsed_pit_ticks > APIC_TIMER_MAX_REFERENCE_TICKS) {
            status = APIC_STATUS_BAD_TIMER_SAMPLE;
            break;
        }

        samples[index].elapsed_count = UINT32_MAX - current_count;
        samples[index].pit_ticks = (uint32_t)elapsed_pit_ticks;

        if (samples[index].elapsed_count == 0U) {
            status = APIC_STATUS_BAD_TIMER_SAMPLE;
        }
    }

    pit_status = pit_stop();

    if (status == APIC_STATUS_OK && pit_status != PIT_STATUS_OK) {
        status = APIC_STATUS_PIT_FAILURE;
    }

    local_timer_reset_hardware();

    if (status == APIC_STATUS_OK &&
        (pit_is_running() || !apic_timer_is_masked())) {
        status = APIC_STATUS_REGISTER_MISMATCH;
    }

    return status;
}

enum apic_status apic_local_timer_calibrate_and_start(
    uint32_t periodic_frequency_hz,
    interrupt_handler_t handler,
    void *context,
    struct apic_local_timer_configuration *configuration
)
{
    struct apic_timer_sample samples[APIC_TIMER_CALIBRATION_SAMPLE_COUNT];
    struct apic_local_timer_configuration candidate;
    uint32_t pit_divisor_value = 0U;
    enum interrupt_status interrupt_status;
    enum apic_status status;

    if (handler == NULL || configuration == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    timer_configuration_reset(configuration);

    if (!initialized) {
        return APIC_STATUS_NOT_INITIALIZED;
    }

    if (local_timer_active) {
        return APIC_STATUS_LOCAL_TIMER_ALREADY_ACTIVE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (pit_is_running() || !pit_route_masked ||
        !local_timer_registers_match()) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    bytes_zero(samples, sizeof(samples));
    status = calibration_collect(samples, &pit_divisor_value);

    if (status != APIC_STATUS_OK) {
        return status;
    }

    status = timer_configuration_build(
        samples,
        APIC_TIMER_CALIBRATION_SAMPLE_COUNT,
        pit_divisor_value,
        LOCAL_APIC_TIMER_DIVIDE_BY_16,
        periodic_frequency_hz,
        &candidate
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    interrupt_status = interrupt_register_handler(
        APIC_LOCAL_TIMER_VECTOR,
        handler,
        context
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return APIC_STATUS_INTERRUPT_FAILURE;
    }

    local_timer_reset_hardware();
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_DIVIDE_CONFIGURATION,
        candidate.divide_configuration
    );
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_LVT_TIMER,
        LOCAL_APIC_TIMER_PERIODIC | APIC_LOCAL_TIMER_VECTOR
    );
    mmio_write32(
        active_local_apic_virtual_address,
        LOCAL_APIC_TIMER_INITIAL_COUNT,
        candidate.periodic_initial_count
    );
    timer_configuration_copy(&active_timer_configuration, &candidate);
    local_timer_active = true;

    if (apic_local_timer_validate() != APIC_STATUS_OK ||
        apic_validate() != APIC_STATUS_OK) {
        local_timer_active = false;
        timer_configuration_reset(&active_timer_configuration);
        local_timer_reset_hardware();
        (void)interrupt_unregister_handler(APIC_LOCAL_TIMER_VECTOR);
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    timer_configuration_copy(configuration, &candidate);
    return APIC_STATUS_OK;
}

enum apic_status apic_local_timer_validate(void)
{
    uint32_t decoded_divisor;
    uint32_t expected_periodic_frequency;
    uint32_t frequency_difference;
    uint32_t lower_sample_difference;
    uint32_t upper_sample_difference;
    uint64_t expected_period_nanoseconds;

    if (!initialized) {
        return APIC_STATUS_NOT_INITIALIZED;
    }

    if (!local_timer_active) {
        return APIC_STATUS_LOCAL_TIMER_NOT_ACTIVE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (pit_is_running() || !pit_route_masked ||
        timer_divisor_decode(
            active_timer_configuration.divide_configuration,
            &decoded_divisor
        ) != APIC_STATUS_OK ||
        decoded_divisor != active_timer_configuration.divisor ||
        active_timer_configuration.calibrated_frequency_hz <
            APIC_TIMER_MIN_DIVIDED_FREQUENCY_HZ ||
        active_timer_configuration.periodic_initial_count <
            APIC_TIMER_MIN_INITIAL_COUNT ||
        active_timer_configuration.requested_frequency_hz <
            APIC_TIMER_MIN_PERIODIC_FREQUENCY_HZ ||
        active_timer_configuration.requested_frequency_hz >
            APIC_TIMER_MAX_PERIODIC_FREQUENCY_HZ ||
        active_timer_configuration.periodic_frequency_hz == 0U ||
        active_timer_configuration.minimum_sample_hz >
            active_timer_configuration.calibrated_frequency_hz ||
        active_timer_configuration.maximum_sample_hz <
            active_timer_configuration.calibrated_frequency_hz) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    expected_periodic_frequency = (uint32_t)(
        ((uint64_t)active_timer_configuration.calibrated_frequency_hz +
            active_timer_configuration.periodic_initial_count / 2U) /
        active_timer_configuration.periodic_initial_count
    );
    frequency_difference = expected_periodic_frequency >
            active_timer_configuration.requested_frequency_hz
        ? expected_periodic_frequency -
            active_timer_configuration.requested_frequency_hz
        : active_timer_configuration.requested_frequency_hz -
            expected_periodic_frequency;
    lower_sample_difference =
        active_timer_configuration.calibrated_frequency_hz -
            active_timer_configuration.minimum_sample_hz;
    upper_sample_difference =
        active_timer_configuration.maximum_sample_hz -
            active_timer_configuration.calibrated_frequency_hz;
    expected_period_nanoseconds =
        ((uint64_t)active_timer_configuration.periodic_initial_count *
            NANOSECONDS_PER_SECOND +
            active_timer_configuration.calibrated_frequency_hz / 2U) /
        active_timer_configuration.calibrated_frequency_hz;

    if (expected_periodic_frequency !=
            active_timer_configuration.periodic_frequency_hz ||
        (uint64_t)frequency_difference * UINT32_C(100) >
            (uint64_t)active_timer_configuration.requested_frequency_hz *
                APIC_TIMER_MAX_FREQUENCY_ERROR_PERCENT ||
        (uint64_t)lower_sample_difference * UINT32_C(100) >
            (uint64_t)active_timer_configuration.calibrated_frequency_hz *
                APIC_TIMER_MAX_SAMPLE_SPREAD_PERCENT ||
        (uint64_t)upper_sample_difference * UINT32_C(100) >
            (uint64_t)active_timer_configuration.calibrated_frequency_hz *
                APIC_TIMER_MAX_SAMPLE_SPREAD_PERCENT ||
        expected_period_nanoseconds !=
            active_timer_configuration.period_nanoseconds ||
        !local_timer_registers_match()) {
        return APIC_STATUS_REGISTER_MISMATCH;
    }

    return APIC_STATUS_OK;
}

bool apic_local_timer_active(void)
{
    return initialized && local_timer_active;
}

bool apic_vector_requires_eoi(uint8_t vector)
{
    return initialized &&
        ((vector == APIC_TIMER_VECTOR && !pit_route_masked) ||
         (vector == APIC_LOCAL_TIMER_VECTOR && local_timer_active));
}

void apic_send_eoi(void)
{
    if (!initialized) {
        return;
    }

    mmio_write32(active_local_apic_virtual_address, LOCAL_APIC_EOI, 0U);
    ++end_of_interrupt_count;
}

uint64_t apic_eoi_count(void)
{
    return end_of_interrupt_count;
}

bool apic_spurious_self_test(void)
{
    uint64_t spurious_before;
    uint64_t eoi_before;

    if (!initialized || cpu_interrupts_enabled()) {
        return false;
    }

    spurious_before = spurious_interrupt_count;
    eoi_before = end_of_interrupt_count;
    interrupt_trigger_apic_spurious();
    return spurious_interrupt_count == spurious_before + 1U &&
        end_of_interrupt_count == eoi_before;
}

bool apic_ready(void)
{
    return initialized;
}

const char *apic_status_string(enum apic_status status)
{
    switch (status) {
    case APIC_STATUS_OK:
        return "ok";
    case APIC_STATUS_NULL_ARGUMENT:
        return "null APIC argument";
    case APIC_STATUS_ALREADY_INITIALIZED:
        return "APIC subsystem was initialized twice";
    case APIC_STATUS_NOT_INITIALIZED:
        return "APIC subsystem is not initialized";
    case APIC_STATUS_INTERRUPTS_ENABLED:
        return "APIC mutation requires IF cleared";
    case APIC_STATUS_INTERRUPT_FAILURE:
        return "APIC interrupt handler operation failed";
    case APIC_STATUS_VIRTUAL_MEMORY_FAILURE:
        return "APIC MMIO mapping does not match firmware topology";
    case APIC_STATUS_UNSUPPORTED_CPU:
        return "CPU does not expose a local APIC";
    case APIC_STATUS_X2APIC_ACTIVE:
        return "x2APIC mode is active but this milestone requires xAPIC MMIO";
    case APIC_STATUS_LOCAL_APIC_DISABLED:
        return "local APIC is globally disabled";
    case APIC_STATUS_NOT_BOOTSTRAP_PROCESSOR:
        return "APIC activation did not run on the bootstrap processor";
    case APIC_STATUS_LOCAL_APIC_BASE_MISMATCH:
        return "APIC-base MSR disagrees with the MADT";
    case APIC_STATUS_BAD_LOCAL_APIC_VERSION:
        return "local APIC version or LVT capacity is unsupported";
    case APIC_STATUS_LOCAL_APIC_ID_MISMATCH:
        return "running local APIC ID is absent from the MADT";
    case APIC_STATUS_BAD_IO_APIC_COUNT:
        return "I/O APIC topology and mapping counts disagree";
    case APIC_STATUS_BAD_IO_APIC_ADDRESS:
        return "I/O APIC virtual address is invalid";
    case APIC_STATUS_BAD_IO_APIC_ID:
        return "I/O APIC hardware ID disagrees with the MADT";
    case APIC_STATUS_BAD_IO_APIC_VERSION:
        return "I/O APIC version or redirection capacity is unsupported";
    case APIC_STATUS_BAD_GSI_RANGE:
        return "I/O APIC GSI range overflows";
    case APIC_STATUS_OVERLAPPING_GSI_RANGE:
        return "I/O APIC GSI ranges overlap";
    case APIC_STATUS_BAD_TIMER_OVERRIDE:
        return "ACPI IRQ0 override is ambiguous or invalid";
    case APIC_STATUS_TIMER_GSI_UNROUTABLE:
        return "no I/O APIC covers the resolved timer GSI";
    case APIC_STATUS_PIC_NOT_MASKED:
        return "legacy PIC did not remain fully masked";
    case APIC_STATUS_REGISTER_MISMATCH:
        return "APIC register readback disagrees with the active contract";
    case APIC_STATUS_LOCAL_TIMER_ALREADY_ACTIVE:
        return "Local APIC timer was activated twice";
    case APIC_STATUS_LOCAL_TIMER_NOT_ACTIVE:
        return "Local APIC timer is not active";
    case APIC_STATUS_PIT_FAILURE:
        return "PIT reference failed during Local APIC timer calibration";
    case APIC_STATUS_BAD_TIMER_DIVIDE:
        return "Local APIC timer divide configuration is invalid";
    case APIC_STATUS_BAD_TIMER_FREQUENCY:
        return "Local APIC timer frequency arithmetic is invalid";
    case APIC_STATUS_BAD_TIMER_SAMPLE:
        return "Local APIC timer calibration sample is invalid";
    case APIC_STATUS_TIMER_TIMEOUT:
        return "Local APIC timer calibration exceeded a bounded wait";
    default:
        return "unknown APIC status";
    }
}
