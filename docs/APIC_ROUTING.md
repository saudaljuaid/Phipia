# APIC interrupt routing

This milestone activates the interrupt-controller topology that Zenith already
validated from ACPI and mapped through the permanent virtual-memory hierarchy.
The legacy 8259 PIC remains initialized only long enough to preserve the older
spurious-IRQ regression proof. Before external interrupts are enabled, both PIC
masks are set to `0xFF`, every I/O APIC redirection entry is masked, and exactly
one timer route is installed.

The PIT is the temporary calibration source. Its IRQ0 signal enters the I/O
APIC selected by the ACPI interrupt-source override, reaches vector `0x20` on
the bootstrap processor's Local APIC, and completes through the Local APIC EOI
register. No interrupt is delivered through the legacy PIC after activation.

## Hardware contract

The register definitions follow the Intel 64 and IA-32 Software Developer's
Manual Local APIC chapters and the Intel 82093AA I/O APIC specification. The
firmware route follows ACPI 6.6 MADT I/O APIC and Interrupt Source Override
semantics.

Zenith requires all of the following before it changes controller state:

- CPUID leaf 1 reports an on-chip APIC;
- `IA32_APIC_BASE` has global APIC enable and BSP set, x2APIC clear, and a base
  address equal to the effective MADT Local APIC address;
- the Local APIC ID agrees with CPUID and names a usable MADT processor;
- the Local APIC is integrated and exposes the required LVT registers;
- every mapped I/O APIC ID agrees with its MADT record;
- every I/O APIC reports a nonzero, selector-addressable redirection capacity;
- each derived GSI interval is finite and no two intervals overlap;
- the resolved IRQ0 GSI belongs to exactly one interval; and
- every Local APIC and I/O APIC virtual address still translates to the expected
  writable, NX, UC physical page.

The I/O APIC's selector/window pair is accessed only while interrupts are
disabled. C `volatile` accesses are intentional here: each load or store is a
device transaction, not ordinary memory. The permanent page-table contract
provides the required uncacheable mapping.

## Activation invariant

Activation runs with `RFLAGS.IF=0` and follows this order:

1. mask and read back both legacy PIC interrupt masks;
2. mask every implemented I/O APIC redirection entry;
3. resolve ISA IRQ0 through the optional MADT override, where conforming ISA
   polarity means active-high and conforming trigger means edge-triggered;
4. program the destination, vector, polarity, and trigger while the route is
   still masked;
5. register vector `0xFF` as the Local APIC spurious vector;
6. lower the task-priority threshold, keep the Local APIC timer and error LVTs
   masked, and set the Local APIC software-enable bit; and
7. read every owned register back before publishing the subsystem as ready.

Starting the PIT installs its vector handler before unmasking the single I/O
APIC route. Stopping it masks and verifies the route before unregistering the
handler. The interrupt dispatcher issues one Local APIC EOI after every handled
timer interrupt. A spurious Local APIC vector deliberately receives no EOI.
All other I/O APIC inputs remain masked. The following timer milestone uses
this exact route for bounded calibration, then masks it before activating the
dedicated Local APIC timer vector described in `LOCAL_APIC_TIMER.md`.

## Executable proof

The pure rejection suite builds synthetic controller observations and proves
rejection of null inputs, unsupported Local APIC versions, a missing bootstrap
processor ID, mapping-count disagreement, I/O APIC ID mismatch, invalid
redirection capacity, overlapping GSI ranges, an uncovered timer GSI, duplicate
IRQ0 overrides, and unaligned device mappings.

Every QEMU boot activates and validates the real emulated Local APIC and I/O
APIC before its scenario begins. The dedicated `apic` scenario additionally
proves that a software-delivered spurious vector returns without an EOI, then
waits for eight APIC-routed PIT interrupts and verifies the EOI count, timer
mask, PIC masks, and complete register readback. The normal boot must emit:

```text
Zenith OS: legacy PIC permanently masked
Zenith OS: APIC interrupt routing verified
Zenith OS: APIC-routed PIT delivered eight interrupts
```

## Deferred work

This remains a single-core xAPIC foundation. The Local APIC timer milestone now
consumes the PIT route as a bounded calibration reference and masks it before
periodic activation. Starting application processors, parsing Local APIC NMI
records, x2APIC, dynamic vector allocation, PCI routing, MSI/MSI-X, interrupt
remapping, and cross-CPU controller synchronization remain deferred.
