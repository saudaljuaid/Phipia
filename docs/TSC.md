# Time-stamp counter reference

The local APIC timer was calibrated against the PIT, which left Seneri with one
clock and no way to check it. This increment adds a second, independently
calibrated clock, so the two can be compared.

## Why a second clock is the point

A single calibrated clock cannot be validated. If its measurement is wrong,
every interval derived from it is wrong together, consistently, and nothing in
the system disagrees. Two clocks measured from the same reference and then
compared against each other can catch that: a rate that is wrong shows up as a
disagreement about how long the same interval lasted.

That is what this increment buys. It is the prerequisite for retiring the PIT,
not the retirement itself.

## Reading the counter

`RDTSC` is not a serializing instruction, so a read may be reordered by a few
instructions relative to the surrounding code. Every interval Seneri measures
with it is milliseconds wide, far larger than that window, and the primitive is
documented as unsuitable for short precise intervals, which would need a fenced
variant of their own.

## Calibration and its refusals

Calibration measures the counter across ten PIT ticks at 100 Hz, the same
reference interval the APIC timer used, and scales the span to a second. Unlike
the APIC timer the counter runs up rather than down, so the refusals differ:

- an end at or below the start means the counter never advanced;
- an end below the start means it ran backwards, which no time base may do;
- a rate above eighteen gigahertz is measurement noise, not a processor, and is
  also the bound that keeps the nanosecond conversion from overflowing.

Calibration happens once. A second attempt is refused, for the same reason as
the APIC timer: a rate that changed under the kernel would silently change the
meaning of every duration derived from it.

## Converting to nanoseconds

The conversion splits the span so neither half overflows: whole seconds scale
directly, and the sub-second remainder is always below the rate, which the
calibration bound keeps small enough to multiply by one billion. A remainder
that is simply divided away would lose everything shorter than a second, so the
two parts are added rather than one being dropped.

An uncalibrated counter and a backwards span both convert to zero, because
neither has an honest duration to report.

## What this does not establish

The processor reports whether its counter ticks at a constant rate through power
and thermal transitions, in `CPUID.80000007H:EDX[8]`. On the supported QEMU
target that bit is **not** set, and Seneri reports it rather than assuming it:

```text
Seneri OS: TSC calibrated at 1053607800 Hz, invariant no
```

So the counter is usable here as a second opinion about an interval measured
moments ago, and it is not yet a time base Seneri may trust across long spans or
power states. Retiring the PIT needs a reference that is trustworthy on its own,
which means either a target that reports an invariant counter or the ACPI power
management timer. Recording the bit now is what makes that decision evidence
driven later.

## Executable proof

`tsc_self_test` proves the arithmetic without hardware, because a mistake there
is silent: durations come out wrong rather than absent. It covers a correct
rate, a counter that never advanced, one that ran backwards, a rate beyond the
representable bound, a zero reference frequency, exact one-second and
half-second conversions, a sub-second remainder that must not be divided away, a
zero span, an hour at three gigahertz converting without overflow, and a
backwards span reporting zero.

The `tsc` QEMU scenario proves the hardware path and the cross-check: an
uncalibrated counter reports no duration, calibration succeeds and cannot be
repeated, sixty-four successive reads never step backwards, and then a span
measured by the TSC is compared against the same interval measured by the local
APIC timer.

Normal boot additionally requires:

```text
Seneri OS: TSC calibrated at <n> Hz, invariant <yes|no>
Seneri OS: TSC reference established
```

A negative control confirms the cross-check is real: multiplying the calibrated
rate by four fails the scenario with `TSC and local APIC timer disagree about an
interval`, rather than passing because the counter advanced at all.

## Deferred work

Retiring the PIT needs a reference trustworthy on its own, as above. Nothing
here is per-processor: a second processor's counter would need its own
calibration and its own agreement check, since neither synchronisation nor a
shared rate may be assumed. Level-triggered I/O APIC routing still needs
directed EOI.
