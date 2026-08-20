# Local APIC timer

Pyrenis's timer interrupt now originates in the processor rather than in a
separate 8254 chip. Getting there required answering one question the hardware
does not answer for itself: how fast does the local APIC timer count?

## Why calibration is unavoidable

The local APIC timer counts the processor's bus or core crystal clock, whose rate
is not reported by CPUID on the processors Pyrenis targets and is not described by
ACPI. A count is therefore meaningless until it has been measured against a
reference whose rate is known.

The PIT served as that reference when this increment was written, which is why it
survived it. It no longer does: `docs/PIT_RETIREMENT.md` moves the calibration
onto the ACPI power management timer and retires the 8254. Read that document
before trusting the numbers below — the PIT's tick accounting turned out to be
wrong by a factor of two, so every rate calibrated against it, including the one
described here, was half its true value until it was corrected.

## How the measurement is taken

The divide configuration selects divide by sixteen, which keeps a one-shot run
from exhausting the 32-bit counter while leaving far more resolution than the
interval needs. The timer is loaded with the maximum count while its local
vector table entry is masked, so calibration raises no interrupt.

The reference then runs for a tenth of a second and the APIC timer's current
count is read before and after. The rate is the elapsed count scaled to a second.
Since `docs/PIT_RETIREMENT.md` that reference is the ACPI power management timer
and the span is scaled by the ticks it actually advanced rather than the ticks
requested, so a bounded wait's overshoot stays out of the rate.

Three outcomes are refused rather than recorded:

- an end count of zero, which means the counter reached the bottom and the
  elapsed count is a floor rather than a measurement;
- a start that is not above the end, which means the counter never ran down;
- a computed rate of zero, which cannot divide into any period.

Calibration happens once. A second attempt is refused, because a rate that
changed under the kernel would silently change the meaning of every interval
derived from it.

## Running

A requested frequency becomes an initial count by dividing the calibrated rate.
A count of zero or one wider than the register is refused, so an unrepresentable
frequency fails at the call rather than producing a timer running at some other
rate. The local vector table entry is programmed in periodic mode and read back
before the count is written, because writing the count is what starts the timer.

The timer's vector sits in a range disjoint from both the 8259 and I/O APIC
ranges, and the dispatcher acknowledges it at the local APIC. Its end of
interrupt follows the handler, as with every other APIC-delivered vector.

## Executable proof

`apic_timer_self_test` proves the calibration arithmetic without hardware,
which matters because wrong arithmetic does not fail loudly: it produces a timer
that runs at the wrong speed forever. It covers a correct measurement, a counter
that never moved, a counter that ran backwards, one that reached zero, a zero
reference frequency, a correct count for a requested frequency, a frequency too
fast to express, one too slow for the register, a zero frequency, and the
refusals for waiting on and stopping a timer that is not running.

The `apic-timer` QEMU scenario proves the hardware path and, more importantly,
the *rate*: an uncalibrated timer refuses to start, calibration succeeds and
cannot be repeated, a second start is refused, and then the APIC timer's tick
count is compared against the PIT running over the same interval. A rate wrong
by more than a factor of two shows up as a tick count that disagrees.

Normal boot additionally requires:

```text
Pyrenis: local APIC timer calibrated at <n> counts per second
Pyrenis: local APIC timer delivered eight interrupts
```

A negative control confirms the cross-check is real: multiplying the calibrated
rate by four makes the scenario fail with `local APIC timer rate disagrees with
its reference`, rather than passing because the timer ticked at all.

## Deferred work

The PIT was the calibration reference when this was written and no longer is.
`docs/TSC.md` covers the next increment, which adds a second independently
calibrated clock so the two can be compared. `docs/PM_TIMER.md` covers the one
after it, which found that the reference described on this page was delivering two
interrupts per programmed period, so the rate measured here was half its true
value until that was corrected. `docs/PIT_RETIREMENT.md` covers the increment that
moved this calibration onto the ACPI power management timer and retired the 8254
altogether. ~~Level-triggered I/O APIC
routing still needs directed EOI.~~ **Fixed**, in `docs/IO_APIC.md`. Nothing
here is per-processor: a second
processor would need its own calibration, since the local APIC timer is
core-local.
