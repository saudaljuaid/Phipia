/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/acpi_util.h>
#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>

/*
 * Intel 82093AA section 3.1: the I/O APIC is reached through an index register
 * and a data window sixteen bytes above it, not as a flat register file.
 */
#define IOAPIC_REGISTER_SELECT UINT32_C(0x00)
#define IOAPIC_REGISTER_WINDOW UINT32_C(0x10)

/* Intel 82093AA sections 3.2.1 through 3.2.4 assign these register indices. */
#define IOAPIC_INDEX_ID UINT32_C(0x00)
#define IOAPIC_INDEX_VERSION UINT32_C(0x01)
#define IOAPIC_INDEX_REDIRECTION UINT32_C(0x10)

/* Intel 82093AA section 3.2.4 defines the redirection entry fields. */
#define IOAPIC_DELIVERY_FIXED UINT32_C(0x00000000)
#define IOAPIC_DESTINATION_PHYSICAL UINT32_C(0x00000000)
#define IOAPIC_POLARITY_ACTIVE_LOW UINT32_C(0x00002000)
#define IOAPIC_TRIGGER_LEVEL UINT32_C(0x00008000)
#define IOAPIC_MASKED UINT32_C(0x00010000)
#define IOAPIC_ENTRY_WRITABLE_MASK UINT32_C(0x0001B7FF)
#define IOAPIC_DESTINATION_SHIFT 24U

/* ACPI 6.6 table 5.25 encodes polarity in bits 0-1 and trigger mode in 2-3. */
#define MPS_INTI_POLARITY_MASK UINT16_C(0x0003)
#define MPS_INTI_POLARITY_ACTIVE_LOW UINT16_C(0x0003)
#define MPS_INTI_TRIGGER_MASK UINT16_C(0x000C)
#define MPS_INTI_TRIGGER_LEVEL UINT16_C(0x000C)

#define IOAPIC_ISA_IRQ_COUNT 16U
#define IOAPIC_MIN_ENTRIES 16U

static struct ioapic_state state;
static bool initialized;

/*
 * The index and window registers are memory-mapped device state: each access
 * must reach the device exactly once and in program order, which is what
 * volatile expresses. A select followed by a window access is one indivisible
 * transaction, so every caller below runs with interrupts disabled.
 */
static uint32_t ioapic_read(const struct ioapic_unit *unit, uint32_t index)
{
    volatile uint32_t *select = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_SELECT);
    const volatile uint32_t *window = (const volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_WINDOW);

    *select = index;
    return *window;
}

static void ioapic_write(
    const struct ioapic_unit *unit,
    uint32_t index,
    uint32_t value
)
{
    volatile uint32_t *select = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_SELECT);
    volatile uint32_t *window = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_WINDOW);

    *select = index;
    *window = value;
}

static uint32_t redirection_index(uint32_t entry, bool high)
{
    return IOAPIC_INDEX_REDIRECTION + entry * 2U + (high ? 1U : 0U);
}

static void mask_entry(const struct ioapic_unit *unit, uint32_t entry)
{
    /*
     * Mask before touching the destination so no interrupt can be delivered
     * against a half-written entry.
     */
    ioapic_write(unit, redirection_index(entry, false), IOAPIC_MASKED);
    ioapic_write(unit, redirection_index(entry, true), 0U);
}

/*
 * Translate an ISA IRQ into the global system interrupt and electrical
 * behaviour the firmware declared for it. Without an override, ACPI 6.6 section
 * 5.2.12.5 says an ISA IRQ is identity mapped, edge triggered, and active high.
 */
static void resolve_isa_irq(
    const struct acpi_topology *topology,
    uint8_t irq,
    uint32_t *global_interrupt,
    bool *active_low,
    bool *level_triggered
)
{
    *global_interrupt = irq;
    *active_low = false;
    *level_triggered = false;

    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];

        if (override->source != irq) {
            continue;
        }

        *global_interrupt = override->global_system_interrupt;
        *active_low = (override->flags & MPS_INTI_POLARITY_MASK) ==
            MPS_INTI_POLARITY_ACTIVE_LOW;
        *level_triggered = (override->flags & MPS_INTI_TRIGGER_MASK) ==
            MPS_INTI_TRIGGER_LEVEL;
        return;
    }
}

static struct ioapic_unit *unit_for_interrupt(
    uint32_t global_interrupt,
    uint32_t *entry
)
{
    for (size_t index = 0; index < state.count; ++index) {
        struct ioapic_unit *unit = &state.units[index];

        if (global_interrupt >= unit->interrupt_base &&
            global_interrupt - unit->interrupt_base < unit->entry_count) {
            *entry = global_interrupt - unit->interrupt_base;
            return unit;
        }
    }

    return NULL;
}

/* The routed topology is kept so masking can find the entry again. */
static const struct acpi_topology *routing_topology;

enum ioapic_status ioapic_initialize(const struct acpi_topology *topology)
{
    if (topology == NULL) {
        return IOAPIC_STATUS_NULL_ARGUMENT;
    }

    if (initialized) {
        return IOAPIC_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (topology->io_apic_count == 0U) {
        return IOAPIC_STATUS_MISSING_IO_APIC;
    }

    acpi_bytes_zero(&state, sizeof(state));

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *described = &topology->io_apics[index];
        struct ioapic_unit *unit = &state.units[index];
        uint32_t version;

        unit->identifier = described->identifier;
        unit->address = described->address;
        unit->interrupt_base = described->interrupt_base;
        version = ioapic_read(unit, IOAPIC_INDEX_VERSION);
        unit->version = (uint8_t)(version & UINT32_C(0xFF));
        unit->entry_count = (uint8_t)(((version >> 16U) & UINT32_C(0xFF)) + 1U);

        if ((ioapic_read(unit, IOAPIC_INDEX_ID) >> IOAPIC_DESTINATION_SHIFT &
             UINT32_C(0x0F)) != described->identifier) {
            acpi_bytes_zero(&state, sizeof(state));
            return IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI;
        }

        if (unit->entry_count < IOAPIC_MIN_ENTRIES) {
            acpi_bytes_zero(&state, sizeof(state));
            return IOAPIC_STATUS_TOO_FEW_ENTRIES;
        }

        /*
         * Two I/O APICs claiming the same global interrupt would make routing
         * ambiguous, and the first match would silently win.
         */
        for (size_t earlier = 0; earlier < index; ++earlier) {
            const struct ioapic_unit *other = &state.units[earlier];

            if (unit->interrupt_base <
                    other->interrupt_base + other->entry_count &&
                other->interrupt_base <
                    unit->interrupt_base + unit->entry_count) {
                acpi_bytes_zero(&state, sizeof(state));
                return IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE;
            }
        }

        state.count = index + 1U;

        /* Firmware may leave any entry unmasked; none of them are Seneri's. */
        for (uint32_t entry = 0; entry < unit->entry_count; ++entry) {
            mask_entry(unit, entry);
        }
    }

    routing_topology = topology;
    initialized = true;
    return IOAPIC_STATUS_OK;
}

enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination
)
{
    struct ioapic_unit *unit;
    uint32_t global_interrupt;
    uint32_t entry;
    uint32_t low;
    bool active_low;
    bool level_triggered;

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return IOAPIC_STATUS_BAD_IRQ;
    }

    if (vector < INTERRUPT_IOAPIC_BASE || vector >= INTERRUPT_IOAPIC_LIMIT) {
        return IOAPIC_STATUS_BAD_VECTOR;
    }

    resolve_isa_irq(
        routing_topology,
        irq,
        &global_interrupt,
        &active_low,
        &level_triggered
    );

    /*
     * A level-triggered source needs its remote IRR cleared on every end of
     * interrupt, which this increment does not implement. Refuse rather than
     * route a source Seneri would deliver once and then wedge.
     */
    if (level_triggered) {
        return IOAPIC_STATUS_LEVEL_TRIGGERED;
    }

    unit = unit_for_interrupt(global_interrupt, &entry);

    if (unit == NULL) {
        return IOAPIC_STATUS_UNROUTABLE_INTERRUPT;
    }

    low = IOAPIC_DELIVERY_FIXED | IOAPIC_DESTINATION_PHYSICAL | vector;

    if (active_low) {
        low |= IOAPIC_POLARITY_ACTIVE_LOW;
    }

    /* Destination first, unmasked vector last, so no partial entry can fire. */
    mask_entry(unit, entry);
    ioapic_write(
        unit,
        redirection_index(entry, true),
        destination << IOAPIC_DESTINATION_SHIFT
    );
    ioapic_write(unit, redirection_index(entry, false), low);

    if ((ioapic_read(unit, redirection_index(entry, false)) &
         IOAPIC_ENTRY_WRITABLE_MASK) != low ||
        (ioapic_read(unit, redirection_index(entry, true)) >>
         IOAPIC_DESTINATION_SHIFT) != destination) {
        mask_entry(unit, entry);
        return IOAPIC_STATUS_READBACK_MISMATCH;
    }

    return IOAPIC_STATUS_OK;
}

enum ioapic_status ioapic_mask_isa_irq(uint8_t irq)
{
    struct ioapic_unit *unit;
    uint32_t global_interrupt;
    uint32_t entry;
    bool active_low;
    bool level_triggered;

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return IOAPIC_STATUS_BAD_IRQ;
    }

    resolve_isa_irq(
        routing_topology,
        irq,
        &global_interrupt,
        &active_low,
        &level_triggered
    );
    unit = unit_for_interrupt(global_interrupt, &entry);

    if (unit == NULL) {
        return IOAPIC_STATUS_UNROUTABLE_INTERRUPT;
    }

    mask_entry(unit, entry);
    return IOAPIC_STATUS_OK;
}

struct ioapic_state ioapic_get_state(void)
{
    return state;
}

bool ioapic_is_initialized(void)
{
    return initialized;
}

/*
 * The override resolver is the part of routing that decides where a legacy IRQ
 * actually goes, and it is provable without hardware. Getting it wrong would
 * program a correct-looking entry for the wrong interrupt.
 */
bool ioapic_self_test(void)
{
    struct acpi_topology topology;
    uint32_t global_interrupt = 0U;
    bool active_low = true;
    bool level_triggered = true;

    acpi_bytes_zero(&topology, sizeof(topology));

    /* No override: identity mapped, edge triggered, active high. */
    resolve_isa_irq(&topology, 7U, &global_interrupt, &active_low,
                    &level_triggered);

    if (global_interrupt != 7U || active_low || level_triggered) {
        return false;
    }

    /* The customary IRQ0 to GSI2 override with conforming ISA electricals. */
    topology.interrupt_override_count = 2U;
    topology.interrupt_overrides[0].source = 0U;
    topology.interrupt_overrides[0].global_system_interrupt = 2U;
    topology.interrupt_overrides[0].flags = UINT16_C(0);
    topology.interrupt_overrides[1].source = 9U;
    topology.interrupt_overrides[1].global_system_interrupt = 9U;
    topology.interrupt_overrides[1].flags = UINT16_C(0x000D);

    resolve_isa_irq(&topology, 0U, &global_interrupt, &active_low,
                    &level_triggered);

    if (global_interrupt != 2U || active_low || level_triggered) {
        return false;
    }

    /* Flags 0x0D are active high and level triggered. */
    resolve_isa_irq(&topology, 9U, &global_interrupt, &active_low,
                    &level_triggered);

    if (global_interrupt != 9U || active_low || !level_triggered) {
        return false;
    }

    /* An unoverridden IRQ keeps its identity mapping beside overridden ones. */
    resolve_isa_irq(&topology, 4U, &global_interrupt, &active_low,
                    &level_triggered);

    if (global_interrupt != 4U || active_low || level_triggered) {
        return false;
    }

    topology.interrupt_overrides[1].flags = MPS_INTI_POLARITY_ACTIVE_LOW;
    resolve_isa_irq(&topology, 9U, &global_interrupt, &active_low,
                    &level_triggered);

    if (!active_low || level_triggered) {
        return false;
    }

    return ioapic_initialize(NULL) == IOAPIC_STATUS_NULL_ARGUMENT &&
        ioapic_route_isa_irq(
            IOAPIC_ISA_IRQ_COUNT,
            INTERRUPT_IOAPIC_BASE,
            0U
        ) != IOAPIC_STATUS_OK &&
        ioapic_route_isa_irq(0U, INTERRUPT_IOAPIC_BASE - 1U, 0U) !=
            IOAPIC_STATUS_OK;
}

const char *ioapic_status_string(enum ioapic_status status)
{
    switch (status) {
    case IOAPIC_STATUS_OK:
        return "ok";
    case IOAPIC_STATUS_NULL_ARGUMENT:
        return "null I/O APIC argument";
    case IOAPIC_STATUS_ALREADY_INITIALIZED:
        return "I/O APIC was initialized twice";
    case IOAPIC_STATUS_NOT_INITIALIZED:
        return "I/O APIC is not initialized";
    case IOAPIC_STATUS_INTERRUPTS_ENABLED:
        return "I/O APIC mutation requires interrupts disabled";
    case IOAPIC_STATUS_MISSING_IO_APIC:
        return "ACPI described no I/O APIC to program";
    case IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI:
        return "I/O APIC identifier disagrees with the ACPI MADT";
    case IOAPIC_STATUS_TOO_FEW_ENTRIES:
        return "I/O APIC cannot redirect the sixteen ISA interrupts";
    case IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE:
        return "two I/O APICs claim the same global interrupt";
    case IOAPIC_STATUS_BAD_IRQ:
        return "interrupt request is outside the ISA range";
    case IOAPIC_STATUS_BAD_VECTOR:
        return "vector is outside the I/O APIC delivery range";
    case IOAPIC_STATUS_UNROUTABLE_INTERRUPT:
        return "no I/O APIC owns that global system interrupt";
    case IOAPIC_STATUS_LEVEL_TRIGGERED:
        return "level triggered routing is not implemented yet";
    case IOAPIC_STATUS_READBACK_MISMATCH:
        return "I/O APIC did not read back its programmed entry";
    default:
        return "unknown I/O APIC status";
    }
}
