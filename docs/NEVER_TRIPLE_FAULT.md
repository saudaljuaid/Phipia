# Never Triple Fault

This milestone gives Zenith a deterministic failure boundary. Every x86_64 IDT
vector is present, every entry reaches a normalized Assembly ABI, and every
unexpected event either returns through a registered handler or emits a bounded
diagnostic and halts. A dedicated double-fault path does not call ordinary C.

## Interrupt-entry ABI

AMD64 64-bit interrupt entry always pushes the complete 40-byte `SS`, `RSP`,
`RFLAGS`, `CS`, and `RIP` return frame, and 64-bit `iretq` pops the `SS:RSP`
pair even when CPL does not change. Privilege transitions and IST selection
change which stack receives that same frame; they do not shorten the same-CPL
shape. Vectors 8, 10-14, 17, 21, 29, and 30 also receive an eight-byte hardware
error code. Their stubs push only the eight-byte vector slot; every other stub
pushes a zero error-code slot followed by the vector slot.

The common entry clears the direction flag, saves all fifteen non-RSP general
registers, snapshots CR2, aligns the live stack to the System V call boundary,
and calls ordinary freestanding C. The original frame pointer is held in a
callee-saved register while C runs. The dispatcher returns the frame pointer to
restore. C accepts either that aligned original ring-zero frame or an alternate
frame selected only after the scheduler commits and proves exact stack
ownership, live generation, running-state provenance, kernel CS/RIP/SS, legal
RFLAGS, the saved return RSP, and aligned bounds. Only then does Assembly load
`RSP` directly from the returned pointer, restore the registers, vector and
error slots, and execute `iretq`.

The stubs' 16 normalized bytes plus the common entry's CR2 and fifteen GPRs
extend the hardware frame to the complete 184-byte C and Assembly ABI. Its size
is statically asserted, and scheduler provenance checks require all 184 bytes to
remain inside the owning task stack. Changing its field order, the Assembly
push order, or the list of error-code vectors is one ABI change and must be
reviewed as such.

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

The bootstrap preemption runtime is also an interrupt-activation gate. A
failure returns the dedicated preemption-initialization status and clears the
reserved reschedule handler slot, so the interrupt subsystem is never
published with a handler backed by an unavailable runtime.

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

`make qemu-tests` boots one kernel under sixteen Multiboot command-line scenarios:

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
14. preemptive scheduler creation, switching, CPU-bound register/RFLAGS proof,
    exit, reaping, rollback, and continued timer/EOI delivery;
15. a genuine non-present page fault on an exact task-stack guard address;
16. a deliberate device-not-available exception after eager XSAVE activation,
    proving the exact #NM fault site and fail-closed xstate poisoning.

Each guest prints exactly one `ZT BEGIN <scenario>` and one matching
`ZT PASS <scenario>`, then writes a scenario-specific value to QEMU's test-only
exit device. The host requires the exact transformed status. Timeouts, ordinary
exit, duplicate markers, stale logs, panic text, and missing markers all fail.
Consequently, a reset, hang, malformed `iretq`, double fault, or triple fault
cannot be mistaken for success.

## Deferred work

This remains a single-core, ring-zero foundation. It intentionally has no
`swapgs`, userspace frame, nested interrupts, SMP state, dynamic
vector allocation, guarded interrupt stacks, or interrupt-safe console lock.
The scheduler changes return-frame ownership only at an outermost eligible
interrupt exit after any required EOI; software reschedule vector `0x31`
requires none. IST, panic, nested, and preempt-disabled exits retain their entry
frame. Userspace and SMP mechanisms must arrive with their own changed ABI and
executable proofs.
