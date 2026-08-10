# Never Triple Fault

This milestone gives Zenith a deterministic failure boundary. Every x86_64 IDT
vector is present, every entry reaches a normalized Assembly ABI, and every
unexpected event either returns through a registered handler or emits a bounded
diagnostic and halts. A dedicated double-fault path does not call ordinary C.

## Interrupt-entry ABI

AMD64 long mode pushes `SS`, `RSP`, `RFLAGS`, `CS`, and `RIP` as eight-byte
values for every interrupt. Vectors 8, 10-14, 17, 21, 29, and 30 also receive a
hardware error code. Their stubs push only the vector; every other stub pushes a
zero error code followed by the vector.

The common entry clears the direction flag, saves all fifteen non-RSP general
registers, snapshots CR2, aligns the live stack to the System V call boundary,
and calls ordinary freestanding C. The original frame pointer is held in a
callee-saved register. Return restores the exact saved stack, registers, vector
and error slots before executing `iretq`.

The C structure is statically asserted to 184 bytes. Changing its field order,
the Assembly push order, or the list of error-code vectors is one ABI change and
must be reviewed as such.

## Descriptor-table invariants

The permanent GDT contains a null descriptor, ring-zero code and data, and one
64-bit TSS descriptor. The TSS owns independent 16 KiB stacks for RSP0, double
fault, NMI, and machine check. IST1, IST2, and IST3 select the latter three.
Vector `0xF0` temporarily shares IST1 solely for a recoverable stack-routing
proof.

The IDT is the first subsystem initialized by C. It is first loaded without IST
selectors while the bootstrap GDT is still active. Zenith then loads the
permanent GDT and task register, patches the live IDT with IST selectors, and
validates GDTR, IDTR, TR, all 256 gates, the TSS, and stack canaries before the
boot parser, memory allocator, or any `sti`.

Canaries detect some downward overflow but are not guard pages. The permanent
map gives the bootstrap and IST storage writable, NX pages, but those linked
arrays do not have unmapped gaps. Guarded bootstrap and interrupt stacks remain
future virtual-memory work; the scheduler's separate dynamic task stacks do
have absent per-slot guards.

## Interrupt-controller invariants

Both 8259 PICs are masked before descriptor work. Initialization remaps them to
vectors `0x20-0x2F` and leaves every IRQ masked. Their synthetic spurious IRQ
paths remain a regression test, but the PICs are then permanently masked and
read back before Zenith activates its discovered xAPIC hardware.

The PIT registers its handler and completes its state before the ACPI-resolved
I/O APIC route is unmasked. IRQ handlers run with nesting disabled and the
dispatcher writes the Local APIC EOI register after the handler returns. The
Local APIC spurious vector has a registered no-op observer and requires no EOI.

Spurious IRQ7 receives no EOI. Spurious IRQ15 receives an EOI only on the master
PIC. Genuine slave interrupts receive slave then master EOIs.

## Double-fault containment

Vector 8 enters a fixed Assembly-only routine on IST1. It performs bounded COM1
polling, writes a fixed VGA diagnostic, and halts. It deliberately avoids the C
dispatcher, allocator, mutable console cursor, formatting, and locks. A failure
inside an elaborate double-fault report would become the triple fault this
milestone exists to prevent.

## Executable proof

`make qemu-tests` boots one kernel under fifteen Multiboot command-line scenarios:

1. normal descriptor and frame validation;
2. breakpoint return with every GPR and the direction flag restored;
3. fatal invalid opcode with an exact fault RIP;
4. non-present page fault at 4 GiB with exact CR2 and decoded error bits;
5. present write fault against the read-only kernel image;
6. present instruction-fetch fault against NX kernel data;
7. recoverable entry through the double-fault IST stack;
8. synthetic spurious IRQ7/IRQ15 paths and eight PIT interrupts delivered
   through the I/O APIC, which also prove repeated Local APIC EOIs;
9. complete APIC register readback, a no-EOI spurious vector, permanent PIC
   masks, and a dedicated APIC-routed timer proof;
10. bounded PIT calibration, periodic Local APIC timer delivery, repeated Local
    APIC EOIs, a masked PIT route, and advancing monotonic nanoseconds;
11. transactional kernel-heap growth, rejection, rollback, exhaustion, and
    continued Local APIC timer delivery;
12. deterministic handling of an unregistered vector;
13. a genuine double fault created by making page-fault delivery fail;
14. cooperative scheduler creation, switching, exit, reaping, rollback, and
    continued timer/EOI delivery;
15. a genuine non-present page fault on an exact task-stack guard address.

Each guest prints exactly one `ZT BEGIN <scenario>` and one matching
`ZT PASS <scenario>`, then writes a scenario-specific value to QEMU's test-only
exit device. The host requires the exact transformed status. Timeouts, ordinary
exit, duplicate markers, stale logs, panic text, and missing markers all fail.
Consequently, a reset, hang, malformed `iretq`, double fault, or triple fault
cannot be mistaken for success.

## Deferred work

This remains a single-core, ring-zero foundation. It intentionally has no
`swapgs`, userspace frame, nested interrupts, SMP state, preemption, dynamic
vector allocation, guarded interrupt stacks, or interrupt-safe console lock.
The cooperative scheduler never changes an interrupt frame; the calibrated
timer supplies time only and does not introduce preemption. Those mechanisms
must arrive with their own changed ABI and executable proofs.
