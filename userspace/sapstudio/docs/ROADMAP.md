<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Roadmap

Smallest first, and each milestone is finished when its evidence exists — not
when its code compiles. A milestone that cannot state a QEMU scenario and a
negative control is not a milestone, it is a branch.

Dates are absent on purpose. The sequence is the commitment.

## M0 — Foundation

*Requires nothing. This is where the repository is now.*

The engineering rules, the open-source map, the platform contract, the planned
architecture, and the hygiene that enforces them.

**Done when:** the documents are merged, `make lint` is green, and the
capability ladder in [`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) has been
read by whoever will implement it in Sapote.

**State: done.**

## M1 — Slate

*Requires `SAP-01`, `SAP-02`, `SAP-06`.*

A native Sapote program, written in Rust, that acquires a surface, draws a
clapperboard and a frame counter, presents once, and exits cleanly. No model,
no media, no interaction. The point is the platform, not the picture.

**Done when:** a QEMU scenario boots Sapote, launches SapStudio from the
read-only volume, matches the presented pixels against a pinned image, observes
a status-zero exit and an equal resource census, and a negative control proves
that a one-pixel mutation fails the scenario. The linked ELF passes the R-13.4
audit: `ET_EXEC`, static, non-PIE, W^X, no dynamic section, no relocations, no
undefined symbols, empty GOT, and no SIMD instruction.

**Also delivers:** the linker script, the build, and `sapstudio-rt` and
`sapstudio-abi` in their first form.

**State: everything that does not need Sapote is done.** `targets/sapstudio.ld`,
the build in the `Makefile`, `sapstudio-rt`, `sapstudio-abi`, and
`sapstudio-image` exist; `make audit` proves the linked image has the shape
Sapote's ELF validation accepts, and `tools/audit-control.py` proves the audit
can refuse. What is left is `SAP-01`, `SAP-02`, and `SAP-06` — the kernel side
— and the picture that `SAP-06` makes possible. Until then the program's
report goes to the console seam, and its whole output is pinned by a golden
transcript test.

## M2 — Model

*Requires nothing from Sapote. Runs entirely on the host.*

`sapstudio-core` and `sapstudio-model`: rational time, timebases, drop-frame
timecode, identifiers, the project structure, the full edit algebra, and the
undo journal. The allocator and the project file format arrive here too.

**Done when:** the R-9.2 property holds under `proptest` for generated edit
sequences of bounded depth; timecode round-trips exactly for every supported
rate including 24000/1001 and 30000/1001 across a full day of frames; the
project file round-trips byte-identically; the format's parser has a fuzz
target with a committed corpus; and a negative control proves a corrupted
project file is refused rather than partially loaded.

This is the largest milestone and the one with no platform risk at all. It can
proceed in parallel with Sapote's work on M1's capabilities.

**State: done.** `sapstudio-core` holds
exact rational time, timebases, half-open ranges, and drop-frame timecode, and
its tests sweep every frame of a whole day at 24, 25, 30, 48, 50, and 60
non-drop and at 30 and 60 drop-frame — some twenty-nine million round trips,
each of which must name its own frame back. `sapstudio-model` holds the
project, sequences, tracks, items, the eight edit operations, and the journal;
the undo property is checked over two thousand generated sessions plus forty
long ones, and a deliberately broken inverse was shown to fail it.
`sapstudio-io` holds the project file: versioned, length-prefixed, and
digested, so that **every single-byte change to a valid file is refused** —
which is a sweep, not a claim. Saving is the four-step protocol in
[`ARCHITECTURE.md`](ARCHITECTURE.md), and each of its four failure modes is a
test that requires the previous file to survive whole; removing the read-back
comparison was shown to fail exactly one of them. The allocator over `SAP-03`'s
pages is what remains, and it needs `SAP-03`.

## M3 — Reel

*Requires `SAP-03`, `SAP-05`, and read-only storage as it already exists.*

Play one deterministic uncompressed clip from the read-only volume: read,
describe, cache frames in a bounded pool, and present them on time.

**Done when:** a QEMU scenario plays a pinned clip and proves every frame was
presented, in order, with its interval measured against `SAP-05`'s clock and
recorded; the frame pool's census is equal at teardown; a negative control
proves a truncated clip is refused with a named error and leaves the
application usable.

**Also delivers:** the `SPRW` uncompressed mezzanine format, the frame pool,
the cache key discipline of R-8.5, and the first golden-hash tests.

**State: the half that needs no operating system is done.** `sapstudio-media`
holds frames that cannot be built without a complete colour description —
there is no `Unknown` primary, no `Unspecified` transfer, no `Default`
anywhere, so the untagged frame that causes every washed-out export in this
industry is not a value this application can construct. Plane arithmetic is
checked at every dimension in a range rather than at three examples. The pool
is bounded in frames and in bytes, evicts by use with deterministic
tie-breaking, and refuses to let one key ever name two different frames. The
`SPRW` mezzanine round-trips every format, and its single-byte sweep found a
real gap — a digest over only the samples let a flipped transfer-function byte
silently change every frame in the file — which is now closed by digesting the
header too. What remains is reading a reel from Sapote's storage and presenting
it on time, and that is `SAP-03`, `SAP-05` and `SAP-06`.

## M3.5 — Interchange

*Requires nothing new.*

CMX 3600, the edit decision list every system still speaks: a text format
designed in the 1970s to drive tape decks, and still how a cut moves between
two applications that agree on nothing else.

Four things about it are traps, and each has a test named after it. **The out
point is exclusive** — `00:00:10:00` as a source out means the last frame used
is the one before it, and an importer that reads it as inclusive makes every
clip one frame too long, which is invisible until something goes to air.
**`FCM` is stateful** — it applies to every event after it until the next one,
so a parser that reads the first and forgets to watch for more mistimes the
second half of the reel. **Drop-frame is stated twice**, once by the `FCM` line
and once by the semicolon before the frames field; when they disagree the file
is refused rather than read one way or the other, because they are two
statements about the same fact. **A reel name is eight characters**, so writing
an EDL loses the identity of the source — deliberately, with the full name in
the comment the format provides for it.

A CMX 3600 timecode has two digits for frames and no field anywhere saying what
rate it counts in, so a 24-frame film cut and a 25-frame PAL cut are written
identically. That is the format's deepest limitation and it is not one a parser
can fix: this one will not invent the rate, and anything that needs it must be
told.

Every prefix of a valid event line is swept and must be refused; lines, event
counts and reel names are all bounded before anything is allocated (R-11.2).

**State: done.** Contiguity is asked one channel at a time — picture and sound
both start at the top of the programme, so a whole-list version of the question
would report every ordinary two-channel list as full of holes, which is true of
the file and false of the cut.

## M4 — Cut

*Requires `SAP-07` and `SAP-08` step one.*

A timeline the user can actually edit: load media, place clips, cut, trim,
ripple, undo, save, reopen, and render the sequence to a file.

**Done when:** a QEMU scenario drives real keyboard and pointer events through
a scripted edit session, saves, reloads, and produces a render whose SHA-256
matches a pinned value; the same project rendered twice produces identical
bytes; a negative control interrupts the save and proves the previous file
survives intact (R-9.4).

This is the first milestone at which SapStudio is, in the narrowest possible
sense, an editor.

## M4.5 — Dissolve

*Requires nothing new.*

The first transition, and the first thing on a timeline that is not a cut.

A dissolve sits **at a boundary** rather than being an item of its own. An item
of its own would have to overlap its neighbours, and this model's guarantee is
that a track's items tile it with no overlap and no missing region — a
guarantee that holds by construction rather than by validation, and is not
worth giving up for one feature.

So a dissolve is a length and the cut it is centred on. During it both
neighbouring clips are on screen: the outgoing one reaching past its out point
and the incoming one starting before its in point, into material each has but
does not otherwise use. That material is called handles, and whether it exists
is a question about the media — which the model does not know, and does not
pretend to. What it *can* check it does: a dissolve may not outlast either clip
it is between, and the incoming clip's in point must be far enough into its
source for the dissolve to reach back at all.

It needs no new operator. The stack reports both sides — the outgoing at full
opacity and the incoming at the fraction of the way through — so `over`
computes `in x t + out x (1 - t)`, which is what a cross-fade is. The fraction
is an exact rational and runs from `1/(N+1)` to `N/(N+1)`, touching neither
end: a dissolve whose first frame is entirely the outgoing clip has spent a
frame showing what the frame before it already showed.

An edit that would renumber a cut a dissolve sits on is refused rather than
silently renumbering the dissolve — take it off first. That is one more step
for the user and one fewer way for a project to end up describing something
nobody edited (R-1.3).

**State: done**, for dissolves and — since **M8.3** — for wipes, which needed
a shape (M8.2) and then a place in the model. A dip to colour needs a colour
source, which is a clip of colour rather than a special case here.

## M4.6 — Keyframes

*Requires nothing new.*

Opacity that fades, a scale that pushes in, a volume that ducks under
dialogue. Each is one number with a different value at different instants, and
a **curve** is the shape it takes between the moments somebody actually set it.

Held at both ends, never extrapolated. Continuing the slope past the last key
is how a parameter set to reach 100% at the end of a shot arrives at 340% two
shots later; the editor who set two keyframes described what happens between
them and nothing else.

Three interpolations. **Hold** keeps the outgoing value and then jumps, which
is what a parameter with discrete settings needs and what an editor reaches for
when a smooth ramp would be a mistake. **Linear** runs straight between, and is
exact: a rational fraction of a rational change is a rational, so a twenty-four
frame ramp is `n/24` at frame `n` on the nose, including the thirds and
sevenths a binary fraction cannot hold.

**Ease** is the cubic Bézier every editor draws as two handles, and it is the
one with an arithmetic problem worth stating plainly. A Bézier is parameterised
by `t`, but a curve is asked for a value at a *time*, and getting from one to
the other means solving `x(t) = time` — a cubic, whose solution needs a cube
root and is therefore not rational. There is no exact answer to find.

So the approximation is stated instead of hidden. `t` is found by bisection to
twenty halvings — one part in a million, far finer than any pixel, sample, or
parameter an editor sets — and the value is then computed at that `t` in 128-bit
integers and rounded once, half away from zero, to the same precision. The
number handed back is the curve's value at a dyadic parameter within `2^-20` of
the right one, with one stated rounding on top. Not "close enough": a size,
written down, and asserted.

Twenty rather than thirty-two because exact arithmetic on a cubic *cubes* the
denominator. A parameter over `2^32` would make `t³` a fraction over `2^96`,
which no 64-bit rational holds — so the count is set by what the arithmetic can
carry rather than by what sounds precise.

The bisection needs `x(t)` monotone, so the handles' *horizontal* positions are
held inside the span and a handle outside it is refused rather than clamped: a
folded curve has more than one value at an instant and no arithmetic will pick
between them. The verticals are not clamped, because a handle past one is an
overshoot — a push that goes a little past and settles — and that is something
an editor asks for on purpose.

One pleasant fact the tests pin: with the horizontal handles at a third and two
thirds, `x(t) = t` identically, so the default ease costs no inversion error at
all.

The first parameter to read one is a **picture track's opacity**. It is set by
an edit like everything else, so it undoes, it redoes, and it is saved; and
`None` is deliberately not the same as a curve holding one, because a track
with no automation has none to read and the difference is what lets automation
be switched off and back on without losing the shape somebody drew.

At the layer stack a track's opacity *multiplies* what the items on it are
doing rather than replacing it. Two things decide what is on screen during a
dissolve inside a fade — how much of the incoming clip is showing, and how much
of the track is — and either one alone throws the other away.

The value is clamped into nought-to-one at the point it is read, because a
curve may overshoot on purpose and nothing can be more than fully opaque. That
is a different decision from refusing an ease handle outside its span: a handle
outside the span changes which curve was drawn, silently, while a value past
full opacity is the curve the editor drew meeting a physical limit.

`SPRJ` moves to version four to carry it. A curve is written after a track's
dissolves as a keyframe count and then, per keyframe, an instant, a value as
its two halves, and an interpolation tag — with an ease's four handles after
the tag. Rationals are written as numerator and denominator rather than as a
scaled integer, because a third is a third and this would otherwise be the one
place in the project file where an exact number stopped being one.

### Sound, where a frame is a stretch rather than a moment

A sound track's fader reads a curve too, in the decibels the fader is already
labelled in — so an automated fader and a static one are the same control. But
picture and sound part company here, and the difference is the whole of the
extra work: a frame of picture is a *moment*, so one value answers it, while a
frame of sound is two thousand samples and a fader may be somewhere different
at each of them.

Applying one gain per frame and holding it flat puts a step at every frame
boundary. On a slow move nobody hears it; on a fast one it is a buzz at
twenty-four hertz, and it is the noise every mixer that ever shipped has had to
be taught not to make. So the mixer takes a **ramp**: a source whose gain runs
from one value at its first sample to another at the sample *after* its last.

That half-open interval is the same shape as everything else here — a frame's
samples run up to the next frame's first, an EDL's out point is exclusive, a
summary block ends where the next begins. It is what makes consecutive blocks
*tile* a fader move: block `n` stops one step short, block `n + 1` starts
exactly there, and no sample is given a gain twice. Closing the interval
instead would repeat one gain at every boundary, and a repetition at a regular
interval is a tone — it would put a hum at the frame rate into every automated
fade.

The interpolation runs in the **factor** rather than in decibels, which is a
choice and is bounded. A fader's travel is logarithmic, so the true path
between two positions is the geometric mean and this takes the arithmetic one:
for a six-decibel move the two differ by half a decibel at the middle of a
block and by nothing at either end. The ends are what matter, and a block is
one frame, so the approximation lives inside a fortieth of a second and shrinks
as blocks do. Interpolating in decibels would mean a logarithm and an
exponential at every sample, at every rate, for a difference no listener meets.

**Mute is not a fader position.** It is a switch, and a curve holds decibels —
its floor is very quiet and is still a level. So a muted track stays muted
whatever is drawn on it, and a track turned off at the surface does not come
back on because somebody drew a fade. That also means mute cannot change within
a block, which is why the mixdown may decide not to decode a muted track from
one end alone; if mute ever gets a lane of its own, that becomes a two-ended
question again.

### Editing one keyframe

Replacing a whole curve is correct and coarse. It undoes, but a journal entry
carries two entire curves, and fifty drags of one keyframe are fifty copies of
everything else on the lane. So there is a second edit that is what a keyframe
*drag* actually is: add one, remove one, move one in time, or change what one
holds.

Adding to a lane with nothing on it **starts** the automation, and removing the
last keyframe **turns it off** — which is what makes those two each other's
exact inverse. An editor who adds a keyframe to a parameter nobody has animated
expects the parameter to become animated, and undoing that expects it to stop,
not to leave a flat curve nobody asked for sitting on the track.

Moving and setting are separate operations because they are separate gestures
with separate risks: a move can reorder the curve and cannot change a value, a
set can change a value and cannot reorder anything. Neither can do the other's
damage by accident. A keyframe may move *past* its neighbours, which is a thing
an editor does on purpose and inverts cleanly, but it may not land *on* one:
two values at an instant is the same nothing as none.

The four operations are nested inside one edit rather than spread across four
more, and that is a structural decision rather than a tidying one. `Edit::apply`
is a dispatch, so its length is the number of operations it dispatches; a subset
of a dispatch needs an arm for every variant the subset does not handle, which
is a branch nothing reaches and no test can cover. Nesting gives one arm at the
top and a match underneath that is exhaustive over exactly four.

**State: done for both lanes.** Not done: curves on items rather than tracks,
which needs a name for a keyframe that survives its item being renumbered; and
automation for anything that is not a level — a pan, a parameter on an effect
that does not exist yet.

## M5 — Sound

*Requires `SAP-13`, and `SAP-10` for comfort.*

Audio decode, sample-rate conversion, a mixer with a declared latency, EBU R128
metering, and synchronised playback.

**Done when:** a QEMU scenario plays a pinned tone sequence and proves
sample-exact alignment against the video presentation clock; underruns are
counted and are zero for the scenario's duration; a negative control forces an
underrun and proves it is reported rather than concealed (R-8.7).

## M5.5 — Mix arithmetic

*Requires nothing new. `SAP-13` is the sound device these buffers reach.*

Everything a mixer needs before it needs somewhere to play. Buffers that carry
their own sample rate, gain in the decibels a fader is labelled in, a pan law
that keeps energy constant across the image, and a sum that says what full
scale cost.

No floating point, for two independent reasons. Sapote preserves no
floating-point state anywhere, so a Ring 3 program has no guarantee it may
execute a single such instruction (`SAP-04`) — and even on a machine that
could, `pow` and `log` are not specified bit-for-bit by IEEE 754, so two
machines mixing one session would bounce different files. A decibel is a
logarithm and a constant-power pan is a square root, and both come from the
same integer arithmetic the transfer curves use.

Three anchors, and none of them approximate. **Zero decibels is exactly one** —
a mixer whose unity is a hair off colours every channel it touches, compounding
down a bus. **Twenty decibels is exactly ten**, which is what the definition
says and the cleanest check that the logarithm and the exponential are
inverses. And **six decibels is not a doubling**: it is 6.020599913, and the
tests measure what rounding it costs rather than repeating the folklore — two
full-scale sources trimmed six decibels each still clip, by a quarter of a
percent.

The pan law is constant power: `left² + right²` is one at every position, so a
source does not change loudness as it crosses the image, and centre is
−3.01 dB rather than 0 or −6. The usual route is a sine and a cosine; this one
substitutes for the angle and needs only a square root, which is exact and
needs no table anyone has to trust. Measured across two thousand positions the
power never moves more than three of the last bits, which is what the rounding
of two roots and two squares allows.

Summing is `i64` throughout, so it is exact: a signal against its own inverse
nulls to precisely zero, adding a muted channel changes a mix bit for bit not
at all, and a bus is the same bus however its channels are scheduled — the same
order-independence argument the render graph makes for pictures (R-6.2).

Full scale is **reported rather than reached quietly**. A mixer that clips in
silence is one whose user finds out on a listening copy; one that refuses
cannot open a session a fader move would fix. So the mix comes back with a
count of the samples it had to write differently from how it computed them, and
by how much.

**State: done.** Resampling, dither and every filter are deliberately absent:
each is a decision with a name, and a mixer that quietly performed one would be
answering a question nobody asked (R-1.3).

## M5.6 — Mixdown

*Requires nothing new.*

The sound half of rendering a sequence, and the place where picture time and
sound time have to agree.

A frame at 24 into 48 kHz is 2000 samples exactly. At 30000/1001 — the
commonest rate in television — it is 1601.6, which is not a number of samples.
No frame at 29.97 holds a whole number of samples and none ever will, so an
implementation that multiplies frames by a constant is wrong before it starts.

A frame's samples are therefore *bounded* rather than counted:
`Instant::floor_into` says which sample a timeline position falls in, and the
samples of frame `n` are those from its floor up to the floor of `n + 1`. Each
block is 1601 samples or 1602, each block's end is the next one's beginning,
and over three hundred frames they sum to exactly 480,480 — arrived at by
arithmetic, not by running the code.

`floor_into` is a separate name rather than a flag on `convert`, because the
two answer different questions: one asks which tick this position *is*, and
refuses when the answer is not a tick; the other asks which tick it is
*inside*, which always has an answer. A caller has to say which it wants
(R-1.3).

Each track mixes at its own fader, and the fader is a property of the
*project*: it is set by an edit, it has an inverse, it undoes, and it is saved.
A mix level that lived only in a function call would be a mix nobody could
deliver.

The model stores the exact decibel value the user set rather than the factor it
becomes, so a fader moved and moved back sits precisely where it started, and a
quarter of a decibel comes back as a quarter of a decibel rather than as
something near it. Muted is a separate detent rather than a very small number,
because the logarithm of zero is not a point on the scale — and a muted track's
media is never read at all, which is the difference between a mix that keeps up
and one that does not.

The travel is written down in both the model and the mixer, because the two are
siblings and neither may depend on the other. The application is the only place
that sees both, so that is where a test asserts they are the same numbers.

The project file goes to version 2 for it, on the same reasoning as `SPRW`: a
version-one file read as version two would take its faders out of what follows
the track kind, which is an item count.

**State: done.** Panning waits for a stage that changes channel counts;
storing a pan nothing applies would be data pretending to be a feature.

## M5.7 — Loudness

*Requires nothing new.*

A peak meter says how close a signal came to the rails and almost nothing about
how loud a programme sounds. Delivery specifications are written in LUFS, so
this is what a delivery is measured against.

Three things in order, as ITU-R BS.1770-4 defines them. **K-weighting**: a high
shelf for the way a head shadows and reinforces sound from in front, then a
high-pass that discards the rumble the ear largely ignores — the coefficients
the standard prints, held as exact decimals. **Mean square, weighted and
summed** across channels, turned into a level with the standard's `-0.691`
offset, which very nearly cancels the weighting's own gain at 1 kHz so that a
tone reads at the level it was recorded at. **Gating** into 400-millisecond
blocks overlapping by three quarters, with an absolute threshold and then a
relative one — without which a minute of silence before the titles would drag a
whole programme down, and the fix would be to trim the silence rather than to
mix it better.

The compliance cases are EBU Tech 3341's own and are *generated* rather than
shipped: a 1 kHz tone at a stated level is exactly reproducible, and generating
it with the integer sine makes the fixture bit-identical on every machine too.
A −23 dBFS tone reads −23.0 and a −33 one reads −33.0, both inside the tenth of
a unit the standard allows.

Everything is integers at forty-eight fractional bits inside the filter. That
is not taste: a biquad is a feedback loop, the high-pass has a pole at 0.995,
and at thirty-two fractional bits the coefficient rounding alone would move it
enough to matter.

**State: done at 48 kHz.** BS.1770 prints its coefficients for that rate and
says to re-derive the filter at others; reusing them would measure the wrong
thing quietly, so another rate is refused (R-1.3). More than two channels is
refused too — the surrounds weigh more than the front, which needs to know
which channel is which, and a buffer carries a count rather than a layout.

## M5.8 — Overview

*Requires nothing new.*

A waveform on a timeline is not the samples. A minute at 48 kHz is nearly three
million numbers and the strip it is drawn into is perhaps two thousand pixels
wide, so every pixel stands for more than a thousand samples and something has
to decide what it stands for. Reading the samples afresh on every scroll makes
scrolling cost the file; deciding badly makes the drawing lie.

So a **summary** is built once and kept: for each block of samples, the lowest,
the highest, and the mean of their squares. Editors call the result a peak
file, and it is what makes a waveform appear the moment a clip lands rather
than a second later.

Two numbers a block rather than one magnitude, because almost nothing is
symmetric. Brass, speech, a kick drum, anything with even harmonics or a trace
of offset goes further one way than the other, and a mirrored drawing is a
picture of a signal nobody recorded — worse, it hides a one-sided approach to
the rails, which is exactly what an editor is looking at the waveform to see.

A **pyramid**, because zoom is continuous and reduction is not free. Level zero
summarises sixty-four samples a block by default; each level above summarises
two blocks of the level below, so the whole zoom range costs under twice the
finest level and any zoom is a read rather than a reduction over a million
samples.

The property everything rests on: **the highest of two highests is the
highest**, exactly. A sample that reached the rails is the maximum of its
block, of the pair containing it, and of every pair above — so zooming out
cannot hide a click, and cannot invent one either. Without that, the way to
find a click you cannot see when zoomed out is to zoom in and scrub the whole
clip, which is the job the waveform exists to save.

The mean square is the one number here that is not exact, because a mean is a
division. The sums fold upward in 128-bit integers and divide once at the end,
so every stored value is one floor from the truth however far out the zoom goes
— not one floor per level. A window of blocks combines stored means and is
within two. Both bounds are asserted, and a floor-of-floors implementation
fails the first.

A summary carries the digest of the sound it summarises, so a stale peak file
is something you can *see* rather than infer from a modification time — which a
copy, a restore, or a clock change will happily lie about.

It is stored as `SPPK`, alongside the project file and the reel. The header
holds the block size, the sample count, the rate, the channel count and the
digest of the sound — and *not* the number of levels or the number of blocks in
each, because both follow from what is already there. Two statements of one
fact is a file that can disagree with itself and a reader that must then decide
which half to believe. The reader computes the shape it expects and reads
exactly that.

The file's own digest covers the header as well as the blocks, and a probe
named what that buys: without it, thirty-three header bytes are undetectably
editable, including all thirty-two of the digest of the sound the summary is
*of*. A staleness check that can itself go stale is not a check.

**State: done, in memory and on the wire.** Not done: writing it anywhere,
which waits on `SAP-08` for a writable filesystem, and the waveform of a whole
sequence rather than of one buffer.

## M6 — Colour

*Requires nothing new.*

The full colour pipeline: tagged frames end to end, explicit conversions,
primaries and transfer functions, scopes, 3D LUT support, and a defensible
Rec. 709 and Rec. 2020 path.

**Done when:** conversions round-trip within a stated tolerance recorded as a
test; every frame in the pipeline is fully described and an untagged frame is
refused (R-8.2); scopes agree with independently computed reference values.

### The gamut half, exactly

A standard defines a gamut with
twelve exact decimal numbers, and everything else — the RGB to XYZ matrix, the
luma coefficients, the matrix from one gamut to another — is derived from them
by linear algebra. `sapstudio-render` does that derivation in exact rationals
rather than in floating point, so there is no tolerance to state: a gamut
converted to itself is the identity matrix, a round trip through XYZ is the
identity matrix, and every row of a conversion sums to exactly one. The
derived BT.709 luma coefficients round to the 0.2126, 0.7152 and 0.0722 the
standard prints, the derived BT.709-to-BT.2020 matrix rounds to the one
BT.2087 publishes, and BT.601's derived coefficients are asserted **not** to
match the 0.299, 0.587 and 0.114 its matrix uses — because those came from the
1953 primaries and were kept when the primaries changed, and an implementation
that derived one from the other would quietly produce a different picture.

Sine and cosine came later and are measured in **turns**, not radians. That is
the whole reason they are exact where it matters: reducing an angle to one
revolution is taking a fractional part, which in binary fixed point is masking
bits and loses nothing — where in radians the same reduction is a division by
an irrational number that loses a little more accuracy for every revolution. So
a quarter turn's sine is exactly one, by construction rather than by luck, and
an angle ten thousand turns out gives bit-for-bit the same answer as the same
angle at the origin. Checked against identities rather than a table: the
squares sum to one at two thousand angles, the sine is odd and the cosine even,
each quadrant has the signs it should, and the addition formula holds.

The transfer functions are done too, and they needed a deterministic power
function first. Sapote has no libm, and even where one exists R-4.1 would not
accept it: `pow`, `exp` and `log` are not specified bit-for-bit by IEEE 754, so
two machines with different libraries produce different pixels for the same
project. So the arithmetic is integers, all of it — `log2` by the classic
bit-by-bit method, `exp2` by multiplying the successive square roots of two
that the fractional bits select, and those roots computed with an integer
square root rather than embedded as a table anyone has to trust. sRGB, BT.709,
BT.2020, both pure gammas, ST 2084 and hybrid log-gamma are all checked against
their standards' own formulas, in both directions, and every curve is checked
to rise everywhere and to undo itself.

The scopes are done as well, and they are measurements rather than pictures:
a histogram and a waveform are counts, so they are exact integers and two runs
over one frame produce identical scopes. Every expectation in their tests is
arrived at by counting rather than by running the code and writing down what it
said — a ramp two hundred and fifty-six pixels wide puts exactly one sample in
every bin, eight bars four rows tall put sixteen samples at each end of every
channel, and a waveform of bars steps down monotonically the way one does on
any monitor in any suite.

The one judgement call is how an RGB frame becomes luminance. A luma-chroma
frame needs no decision — plane zero already is luma — and an RGB frame has no
matrix coefficients to use, so its primaries decide, through the same exact
derivation. The weights are integers that sum to exactly 65,536, because if
they did not a white field would not measure as white.

Frames convert, which is what makes all of the above do something. The order
is the one colour science requires: normalise by range, matrix out of
luma-chroma if there is one, decode to linear light, change gamut **in linear
light and nowhere else**, encode, matrix back, quantise. Quantisation is a
binary search over the transfer table for the code value whose light is
nearest, which is what quantisation means, rather than an encode-and-round.
The legal code range is part of that table: limited range does not merely
scale differently, it forbids the values outside it, and a table that searched
the whole byte answered black with zero — an illegal sample. That was a real
bug, found by a test that asserted the range rather than the round trip.

A scaler and a chroma filter are refused rather than guessed, because each is
a filter and a filter is a decision with a name.

The graph is what the timeline renders *through*, rather than an abstraction
waiting for a caller. A node's key is a digest over its kind, its parameters
and its inputs' identities, so a pool kept between renders answers for anything
already seen — most usefully a source, where the cost avoided is a decode.
Scrub back over a cut and nothing is decoded twice. And because the graph names
media by content digest rather than by any project's index for it, two
sequences cutting the same footage share one cached frame, and a file swapped
underneath is a different key rather than a stale hit.

Compositing is done, and it is the part of a colour pipeline that most often
looks nearly right. Two questions have to be answered before a single sample
is touched, and both are answered here rather than assumed. *In what space?*
Alpha is coverage, so what reaches the eye is a fraction of the top layer's
light plus the rest of the bottom layer's — a sentence that is only true of
light. Every sample is decoded, composited, and encoded back, whatever the
frames are encoded in, and a test computes one such pixel by hand: full white
at coverage 128 over a mid-grey background is code value 205, where adding the
code values instead gives 252, forty-seven too bright. *Straight or
premultiplied?* `over` is correct and associative only on premultiplied
values, so a frame says which it holds, and one that claims premultiplied
while carrying colour brighter than its own coverage is refused — that is the
dark fringe arriving with a note saying it is not one.

`over` is checked as the algebra it is: a transparent layer over anything
leaves it bit-for-bit unchanged, an opaque layer hides everything beneath it,
anything over an opaque layer is opaque, and the two ways of grouping three
layers agree to within one code value, so grouping clips into a compound
cannot change the picture. Premultiplying and dividing back out is stated
honestly as the lossy operation it is: exact at full coverage, within two code
values at half, and total at zero, where the colour is genuinely gone.

Compositing found a real bug in the conversion path that had been passing its
tests. `convert` was writing 255 for every alpha byte, so any keyed frame that
crossed a colour space came out a solid rectangle — and the test that should
have caught it converted opaque bars, which have nothing to lose. Coverage is
now carried through untouched, and a conversion that would gain, lose, or
re-associate alpha is refused, because each of those is an operation with its
own name.

The vectorscope is done, and two things about it are exact rather than pinned.
Neutral is the origin: every grey, at every brightness, under every matrix, has
zero chroma on both axes — checked at a thousand levels for three matrices,
because that property is what makes the middle of the graticule mean "no
colour". And the primaries land on the axes: full red has a red-difference of
exactly one half, cyan exactly minus one half, blue and yellow the same on the
other axis, in BT.601, BT.709 and BT.2020 alike, because `Cr = (R' - Y')/2(1 -
Kr)` and full red makes `Y' = Kr`, so the coefficient cancels itself. Those are
the graticule's fixed marks on every vectorscope ever built, and here they are
derived rather than looked up.

Green and magenta are *not* fixed — they move with the coefficients, which is
why a colourist can tell BT.601 material from BT.709 by where the green box
lands, and why the scope asserts both: a control that replaced the derived
matrix with a hardcoded one left the axis test passing and failed the green
one, which is what that test is for. Chroma is computed from gamma-encoded
samples rather than from linear light, because `Y'CbCr` is defined on the
encoded signal and the primes in its name are the point.

It found a bug on its first run. `Frame::blank` filled every byte with zero,
which is not black: in a limited-range luma plane zero sits below the legal
floor of sixteen, and in a chroma plane zero is not neutral but the most
negative value the byte can hold. A blank frame was a saturated blue-green with
an illegal luma, and it showed up in the corner of the graticule instead of at
the origin. A blank frame is now an opaque black slug, legal in its own range.

### Lookup tables

A look — a film emulation, a show LUT, a camera's log-to-display transform — is
a function from colour to colour that nobody can write down as an equation, so
it is *sampled*: a cube of `size³` output colours and an interpolation between
them. Thirty-three to a side is the common case, which is 35,937 samples for a
function of three variables.

Between eight surrounding samples there is more than one defensible answer, and
the two in use are not equally good. **Trilinear** takes all eight corners of
the cell weighted by position, which is the obvious generalisation and has a
fault on the one part of the picture everybody looks at. **Tetrahedral** cuts
the cell into six tetrahedra along the plane diagonals, picks the one holding
the sample by the *ordering* of the three fractions, and interpolates between
its four vertices — and on the neutral axis those four terms telescope:

```text
c000 + (c100-c000)f + (c110-c100)f + (c111-c110)f  =  c000 + (c111-c000)f
```

which is a straight run between the cell's two diagonal corners. So if a table
is neutral along its own diagonal, **every grey stays grey** — exactly, and in
fixed point, because all three channels then evaluate the identical expression
and round identically. Trilinear mixes all eight corners and drifts off neutral
wherever a table has cross-channel content, which every real look does. A grade
that tints the greys is the first thing a colourist notices.

Both are implemented, and trilinear exists to be *failed*: one test runs a
table that is neutral on its diagonal and lively off it through both, and
asserts tetrahedral holds all thirty-nine greys while trilinear tints
twenty-nine of them. A design decision with no test showing what the rejected
option does is a preference rather than a decision. Trilinear stays available
because reproducing another system's output bit for bit is a real requirement,
and a different one from wanting the better answer.

Fixed point rather than exact rationals, and the reason is worth stating: a
lookup table is a *sampled* function, so the error from sampling a smooth
transform at thirty-three points is orders of magnitude larger than anything
`Fixed` loses. Exact rationals here would be precision spent where it cannot be
observed, paid for with a greatest common divisor on every operation, per
pixel. Where exactness *is* observable it survives anyway and is checked rather
than assumed: a sample on a lattice point comes back untouched because every
weight is nought or one, and a neutral input stays neutral because the three
channels compute the same thing.

`.cube` is the file a colourist hands over — Adobe and Iridas's text format,
and the one every grading system reads and writes. It has traps, and each one
gets a test that fails when the trap is sprung.

**Red varies fastest**, and reading it the other way transposes the cube. A
transposed grade is not a crash or a garish mess; it is a *plausible* picture
with the wrong look, which is the worst kind of wrong for a format to be.

**The numbers are decimal text**, which means they can be read *exactly*:
`0.123456` is `123456/1000000` and nothing is lost. Going through a binary
floating-point type would throw that away on the way in, for no reason, in a
project with no floating point anywhere else.

**`DOMAIN_MIN` and `DOMAIN_MAX` are not always nought and one.** A table
authored for another input range, applied as though the range were the unit
interval, is the wrong look on every pixel — silently. So the domain is read
and refused by name rather than skipped like an unknown key, which is exactly
what a lenient reader would do.

**Output values may leave nought to one and must not be clamped**, because a
look can send a highlight above white on purpose. **`LUT_1D_SIZE` is a
per-channel curve in the same file extension** and is refused rather than read
as a cube. And **scientific notation is refused rather than guessed at**: files
in the wild carry it, saying so plainly is better than reading `1e-3` as one,
and adding it is a small change on the day a file that needs it turns up.

One limitation is the format's rather than the reader's, and it is recorded
because it is an argument for something this project already does. `.cube`
carries no length and no digest, so a file truncated *inside its final number*
still spells three numbers and is accepted — `200.0 0.0 200.0` cut to
`200.0 0.0 200.` even has the same values. `SPRJ`, `SPRW` and `SPPK` all refuse
*every* prefix, because they all carry both. An interchange format without them
has a class of corruption that is undetectable by construction, and a `.cube`
is therefore untrusted input rather than a record.

### A clip carries a grade

A clip holds the **digest** of a look rather than the look, for exactly the
reason the render graph names media by digest: the same grade in two projects
is the same grade, a project-local handle would cache it twice, and a file
swapped underneath a handle is a different look wearing the same name. It is
also the only shape the layering permits — a cube lives in `sapstudio-render`,
which sits *above* the model, so holding one in a clip would invert the
layering as well as the size.

It travels. Through a trim, through a slip, through a save, and through a
split — which gives *both* halves the look, because otherwise joining them back
would not give the clip that was there and join is defined as the exact inverse
of split. Two clips of one piece of media, adjacent in its source and graded
differently, are refused rather than joined: they are two shots with two looks,
and joining them would keep the first's and discard the second's without saying
so.

`SPRJ` moves to version six, a flag byte per clip and thirty-two more when
there is a look. A flag rather than a fixed field, because most clips have no
grade and an all-zero digest is a real digest of something rather than a spare
value to spend on "none".

The render node that reads it is `Node::Look`, which names its look by digest
and fetches it from the library exactly as a source names its media. A grade
edited between two renders is therefore a different cache key rather than a
stale hit — the fault that never gets reported as a bug, because it looks like
the grade not having been saved.

A graded layer is fetched **straight** and associated afterwards. A look is a
non-linear function and on premultiplied samples computes `f(αc)` where
`α·f(c)` was wanted; `over` is only correct on premultiplied ones. The two want
opposite things, so the frame arrives the way the look needs it and
`Node::Associate` puts it back — which loses nothing, because it was never
premultiplied. Unpremultiplying one that had been would, which is why
`Look::apply` refuses rather than doing it quietly.

### Which space a look is applied in

The decision that was left open, now made. A look is authored **for** an
encoding: a show LUT built for a camera's log curve expects log-encoded values,
a display LUT expects display-referred ones. The cube carries no record of
which — it is 35,937 triples — so applying one to the wrong encoding is the
wrong look on every pixel, with nothing crashing and nothing to compare
against.

So a `Look` pairs a table with the description it was authored for, and a frame
that does not match is refused by name. Converting the frame first is the
caller's decision to make and to name, and `convert` is how it is made.

**Not in linear light, and that is not an inconsistency.** The compositor works
in linear light and nowhere else, because `over` is a statement about how much
light reaches the eye and that sentence is only true of light. A lookup table
is the opposite case: it is a sampled function of *code values*, authored by
somebody looking at code values, and decoding to light first would feed it
inputs it was never sampled at. Both rules come from the same place — apply an
operation in the space its definition is written in — and they point in
different directions because the two definitions do.

**Straight coverage only.** A table is a non-linear function applied per pixel,
and on premultiplied samples that computes `f(αc)` where the answer wanted is
`α·f(c)`. Those agree only when `f` is linear or `α` is one — and full coverage
is exactly what a test made of opaque bars would cover, which is how the same
mistake reached the conversion path once already. Unpremultiplying here would
be worse than refusing: it is lossy, so doing it silently would spend a real
quantity of the caller's picture on a step nobody asked for.

**State: done.** M6 is now what its opening paragraph claims.

This milestone said "done" while 3D LUT support, which its own opening
paragraph lists, did not exist in a single line of code. It also carried two
"State:" lines that disagreed with each other. Both are corrected above. A
roadmap that overstates is worse than one that is behind, because the second is
a schedule and the first is a wrong map.

## M8.1 — Conform

*Requires nothing new.*

M3.5 taught this application to read and write CMX 3600. This teaches it to
read and write a **cut** — the layer that knows a track from a channel, a clip
from an event, and a dissolve centred on a cut from one written as a frame
count beside an incoming source.

It is one claim, stated as a theorem rather than as a hope: **if an export
leaves nothing behind, writing its list, parsing it, and importing the result
produces a sequence equal to the one that went in.** Equal by `PartialEq` on
the whole value — not a hand-written comparison of the fields somebody
remembered to check, which is the version of this test that passes forever
after the thing it should have compared stops being compared.

### What is lost and what is refused

That theorem needs an honest account of when it does not apply, and the line
it draws is this milestone's one real decision.

A grade, a fader, an automation curve — each of those *decorates a cut that is
still correct without it*. They are counted and reported, and the list is
written. A second picture track is not a decoration: the format has one video
channel, so a second track's pictures would be written as though they replaced
the first's, and what comes back is a different programme rather than a plainer
one. That is refused.

The test of which side a thing falls on: after a lossy export the frames are in
the right order at the right times and only look wrong; after a refused one
they would not have been.

Five things are reported rather than refused — grades, automation curves,
faders including mute, tracks with no clip on them at all, and a track's
trailing gap. Each has two tests: that it is reported, and that a sequence
carrying it really does come back different. A reported loss that turned out to
be no loss would be a warning nobody should heed, and a warning nobody heeds is
worse than none.

### The reel name is the source digest

Eight characters is what the format holds, so the reel is the first eight
characters of the media's content digest and the whole sixty-four goes in the
`FROM CLIP NAME` comment. Three things follow, and the third was a surprise.

The comment is how an import knows what a source *is*. Without it there is
nothing to look up, so it is refused rather than invented — which means this
conforms lists **this application wrote**. Anyone's list still parses; matching
somebody else's reel names to a media library is a question for a person, and
M8 is where a person gets asked.

The reel and the comment are then two statements about one fact, so when they
disagree the list is refused — the same rule the parser already applies to
drop-frame being stated by both the `FCM` line and the punctuation.

And two sources whose digests agree in those eight characters are one reel name
for two pieces of media. Thirty-two bits is not an identity; the export refuses
rather than writing a list that nothing, including this, could read back
correctly.

### A dissolve, twice

The model centres a dissolve on its cut and opens half its length, rounded
down. CMX writes one as a frame count on the *incoming* event, whose record in
is where the dissolve begins — so the outgoing event stops early, by exactly
that opening, and the frames between are stated by the dissolve rather than by
either event.

Both directions are exact, including for an odd count: twenty-five frames opens
twelve before the cut and thirteen after, and the list leans the same way or
the picture moves by a frame. The model's own bounds are what make the export
total — a dissolve is never longer than its neighbours and its incoming clip
always has room for handles, both checked when it is added — so there is no
arithmetic here that can run off an end, and no branch for one that no test
could reach.

### The rate is still not in the file

M3.5 said the parser will not invent a rate, and it does not: it labels every
timecode at thirty and says so. Which means a label arriving from it carries a
rate that is a *placeholder*, and an importer that asks it for a frame number
as it stands counts a twenty-four frame cut at thirty and stretches the
programme by a quarter.

This module did exactly that on its first run. What caught it was that the
round-trip test goes through the **text** — export, write, parse, import. The
tests that handed the exported list straight back to the importer all passed,
because that is not a round trip; it is a comparison of a value with itself.

So every label is relabelled at the rate the caller stated before it becomes a
number, and a label that cannot count at that rate — a frames field of 27 in a
cut said to be at 24 — is refused rather than taken. There is one backstop
beyond that, and it is not in the file: the cut is built out of media whose
rate the library knows, so a film cut conformed as PAL is refused by the model.
The file has no objection to make. The project does.

**State: done.** Not done: matching another system's reel names to a library,
which needs a person; wipes, which need a shape (M4.5); and lists at rates
whose frames field passes thirty — 48, 50 and 60 write correct files that this
parser cannot be told how to read, which is checked here rather than merely
admitted.

## M8.2 — Shapes

*Requires nothing new.*

M4.5 recorded why wipes were not built: *a wipe needs a shape and a shape needs
a rasteriser*. This is that rasteriser, and it is the same primitive a mask
needs, so it is built once and used twice.

### Coverage is an area, not a sample

The usual rasteriser asks whether a pixel's centre is inside the shape and gets
a jagged edge, or asks at sixteen sub-positions and gets seventeen possible
answers where a byte holds two hundred and fifty-six. This one computes the
**exact area** of the pixel square lying inside the shape, as a rational, and
quantises once at the end.

That is worth the arithmetic for a reason that shows up immediately in motion.
A wipe is one straight line crossing a whole frame over a second, so the same
edge is drawn at a thousand slightly different offsets. Sampled coverage makes
the edge *crawl* — each pixel flips between two values at a different moment —
while an exact area makes it slide, because the area under a line is a
continuous function of where the line is and a sample is not. The difference is
not subtle and it is not a matter of taste; it is the difference between an
edge that looks like an edge and one that looks like a staircase shivering.

A convex shape is an intersection of half-planes, and coverage is found by
clipping the pixel square against each in turn and taking the shoelace area of
what survives. One edge is a wipe, four are a rectangle, many are a mask.

### Two implementations, one of them to be checked against

There is also a closed form for the single-edge case — the area of a unit
square under a line, in three lines of arithmetic — and the rasteriser does not
call it. It exists to be compared against, pixel for pixel, over whole frames
at six orientations: axis-aligned both ways, both diagonals, and two shallow
angles. Two implementations sharing no code and agreeing on all six hundred and
forty-eight pixels is a far stronger statement than one implementation and a
test written after reading it. It is the same argument [`crate::lut`]'s
trilinear path makes, for the same reason.

A third check comes free from a shape that allows one: for an axis-aligned
rectangle the covered area of a pixel is exactly how much of its width overlaps
times how much of its height does, and the general clipper has to agree with a
product it never forms.

And the strongest of them is a relationship rather than a bound: **a shape's
coverage summed over the picture is the exact area the shape encloses in it.**
A rectangle of 3½ by 3¾ sums to exactly 105/8. Any per-pixel error at all — a
wrong partial, a double-counted edge, a column off by one — moves that total.

### A wipe needs no operator of its own

Nor does it, and for the reason a dissolve does not. `faded` scales a
premultiplied frame by one opacity; `masked` scales it by a different one at
every pixel. Mask the incoming layer with the coverage plane, put it `over` the
outgoing one, and the result is `in x coverage + out x (1 - coverage)` — a
cross-fade whose fraction varies across the picture instead of over time.

### What it refuses, and what it costs

Every coordinate is an exact rational and every clip is exact, so denominators
grow: an intersection point is a ratio of products of the inputs and the
shoelace area multiplies those together again. A shape whose arithmetic will
not fit is **refused by name**. There is no floating point to fall back to,
which is the point rather than a limitation.

Two things were written down here as load-bearing and turned out not to be, and
both are now recorded as what they are. That the boundary line belongs to the
region is a decision **with no consequence in this module** — making it strict
changes no coverage anywhere, because a line has no area and the clipper puts
back as a crossing point exactly the corner a strict test would drop. And the
early exit when a polygon has been clipped to fewer than three vertices is an
optimisation, not a correctness guard: the shoelace sum over two vertices is
already nought. Both are kept, and both say so.

**State: done**, for the geometry. Not done here: wipes in the *model*, which
need a transition to say what kind it is — **M8.3**. A soft edge was recorded
here as "a much larger case analysis"; that was **wrong**, and **M8.4** says
why and builds it. And CMX wipe pattern numbers stay
refused on import, now for a sharper reason than "no shape": the shape exists,
and the mapping from a machine's pattern number to it is a convention of that
machine rather than anything the file states.

## M8.3 — Wipes

*Requires nothing new.*

M8.2 built the geometry; this gives it somewhere to live. A `Transition` used
to be a boundary and a length, and reading one back could only produce a
dissolve because a dissolve was the only kind there was. It now says which.

### A wipe is a dissolve that spends its fraction differently

That is the design in one sentence, and everything follows from it. A wipe and
a dissolve at the same cut with the same length are **timed identically**: the
same frames have both clips on screen, and the exact fraction that decides how
far a dissolve has faded decides how far a wipe's edge has travelled. A test
compares the two stacks frame by frame across a whole programme and requires
them to agree about everything except what the fraction is *for*.

So a dissolve spends the fraction on the incoming layer's opacity, and a wipe
carries it. Both of a wipe's clips stay **whole** — the incoming one is not
half-faded, it is entirely there behind an edge — and anything that spent the
fraction twice would show it through the outgoing clip on the covered side.

A track's automation still multiplies, because that is a different question: a
wipe inside a fade has the transition deciding which side of the edge a pixel
is on and the track deciding how much of the whole track is showing, and either
alone throws the other away. The same argument M4.6 makes about a dissolve
inside a fade, met again and answered the same way.

### The direction is a vector, not an angle

An angle needs a sine and a cosine, and neither is exact. A rational vector
needs nothing. Every interface that offers an angle can turn it into a vector
at the moment somebody sets it, so the approximation — if there is one —
happens once at the edge of the system rather than on every frame of every
render.

It also makes the wipes anybody actually uses exact: straight across is
`(1, 0)`, straight down is `(0, 1)`, and a true diagonal is `(1, 1)` rather
than a rounding of forty-five degrees. The vector's *length* carries no
meaning, because the transition's fraction sets the edge's position — so
`(2, 0)` and `(1, 0)` are the same wipe, which is a test, and a normalisation
creeping in would need a square root and would not be exact.

Both ends are reached exactly, by geometry rather than by arithmetic: at nought
the edge sits on the corner the direction points away from and covers nothing,
at one it has passed the opposite corner and covers everything. A transition
showing a sliver of the incoming clip before it started would waste the same
frame the dissolve's fraction is shaped to avoid.

### The graph carries the edge, not the plane

`Node::Wipe` holds four rationals — a direction and a fraction — rather than
the coverage plane they produce. A node's identity is a digest over its
parameters, and a plane of a million bytes would hash into it at a million
times the cost, for a value that is a pure function of the four numbers and the
frame's own size. So the plane is computed at evaluation and the node stays
small enough to be a cache key.

### What moved in the file

`SPRJ` goes to **version seven**. A transition writes its boundary, its length,
then a **tag**, then whatever that kind needs — a wipe's two rationals, a
dissolve's nothing. The tag is before the parameters rather than after so that
a third kind adds a tag rather than changing what the bytes after the duration
mean; version six wrote no tag at all, which is exactly the shape that makes
adding a second kind a break rather than an extension.

An unknown tag is refused by name. Reaching that check needs a **resealed**
file — the byte sweep in this format's tests proves the digest refuses a
mutated byte long before any field is parsed — so the test recomputes the
payload digest around the changed byte, which is what a reader has to survive
being handed by something that meant it.

The slate's golden digest moved, and only because of the version. That was
checked rather than assumed: encoding one transition-free project under six and
under seven produces files of the same length differing in **exactly one byte,
at offset four**. The digest the slate prints is over the whole file, header
included, so a version bump moves it and a payload that did not change is still
a payload that did not change.

### Conforming a wipe

CMX has a wipe event, and it names the shape by a **pattern number** — a
convention of whichever machine wrote the list rather than anything the format
defines. Inventing one would write a file naming a shape this application
refuses to read back, so a wipe is exported as the dissolve it is timed like:
every frame lands where it belongs and the edge is gone.

That is a decoration by M8.1's own line — the cut is still correct and only
looks wrong — so it is reported in `LeftBehind` rather than refused, with the
same two tests every other reported loss has: that it is reported, and that a
sequence carrying it really does come back different.

**State: done**, and **M8.4** adds the soft edge this section listed as
missing. Still not done: wipes with a shape other than a straight line, which
the rasteriser can already describe and the model has no vocabulary for; and
importing somebody else's wipe patterns, which needs a table of one machine's
conventions rather than anything derivable.

## M8.4 — Soft edges

*Requires nothing new.*

M8.2 left a soft edge out and gave a reason: it is "the area weighted by a ramp
rather than a plain area", which is "a much larger case analysis". **The reason
was wrong.** It is two clips and a moment, it is exact, and there is no case
analysis in it at all.

### Why it is exact

**The integral of an affine function over a polygon is its area times its value
at the polygon's centroid.** That is the definition of a centroid rather than a
result about it.

A soft edge is a linear ramp between two parallel lines. The ramp is affine.
The region it runs over is the pixel square clipped by those two half-planes —
convex, because the complement of a half-plane is a half-plane, so the machinery
M8.2 already has produces it. And a convex polygon's area and centroid are
rational. So the coverage of a pixel is the area the near line already covers,
at full weight, plus the area of the slab between the lines weighted by where
in it that slab lies. Exactly.

One arithmetic nicety: the *first moments* — area times centroid — are what the
answer needs, and they are computed without ever dividing by the area. The
division would only be undone by the multiplication that follows, and not doing
it keeps the arithmetic one step shorter and leaves no degenerate case when a
slab has no area.

### What it is checked against

The properties are equalities between rationals, not tolerances.

**A soft edge and its complement still partition every pixel exactly.** The
ramp is symmetric, so what it takes from one side it gives to the other — at
every pixel, not on average over the picture. That is what lets a soft wipe be
built from one plane and its complement without the two disagreeing about a
code value somewhere along the edge.

**A pixel entirely inside the band is the ramp at its centre.** The integrand
is affine over the whole square there, so the integral is the value at the
square's own centroid — a closed form to check the moment arithmetic against.

**Softening an axis-aligned wipe conserves the total coverage exactly.** The
ramp is antisymmetric about its centre line, and over a rectangle the
cross-section along an axis-aligned sweep is the same everywhere, so the two
sides cancel: softness moves coverage around the picture without creating or
destroying any. The condition is real and is stated — a diagonal sweep meets
triangular cross-sections near the corners, the sides do not cancel, and the
total genuinely moves.

**And a vanishing band already agrees with the hard edge.** Nought delegates to
the hard path rather than dividing by a band of nothing, and it would be easy
to assume that delegation papers over a jump. It does not: a band a thousandth
of the travel wide produces the same plane byte for byte, so the soft path
*converges* on the hard one. That started as a control, which passed — and the
reason it passed was worth more as an assertion than the control was as a
control.

### Softness is a fraction of the travel

Not a distance in pixels. It means the same thing at every size and every
angle, and a project conformed to a different frame size keeps the look
somebody set rather than a pixel count that no longer corresponds to it.

`SPRJ` goes to **version eight** — a wipe now writes three rationals rather
than two. Version seven lived for exactly one commit, which is the right
outcome rather than an embarrassing one: the alternative was to redefine what
seven means, and a version whose meaning changed is worse than a version
number nobody spent. The format test uses a softness of **one third**, which is
the value that catches any storage that is secretly binary.

**State: done.** **M8.5** puts the same coverage machinery on a clip as a
mask. Not done here: a soft edge whose ramp is anything but linear —
a smoothstep is not affine, so the centroid identity does not apply and it
would need genuine numerical integration, which is the point at which this
would stop being exact and would have to say so.

## M8.5 — Masks

*Requires nothing new.*

The rasteriser has been able to describe a convex region since M8.2 and the
model had no vocabulary for one. A mask is to *where* what a grade is to
colour: a property of the clip, carried by the layer stack, applied by the
renderer — the same exact-area coverage a wipe is made of, pointed at a clip
instead of at a transition.

### In fractions of the frame

Every corner is a fraction of the width and height, so a mask drawn on a proxy
is the same mask on the finish and a project conformed to another size keeps
the shape somebody drew rather than a pixel count that no longer means
anything. A corner outside nought to one is *not* refused: a mask whose points
sit off the frame is how an editor says "all the way to the edge and past it".

The winding is not the caller's problem either. A polygon's edges point inward
on one winding and outward on the other, so this measures which from the sign
of the polygon's own area rather than demanding one. Getting that wrong inverts
the mask, which is the single most confusing failure a mask can have, and it is
a test: the same shape given both ways round is the same plane.

Inverting flips the **byte**, not the shape. Rasterising the complement instead
would quantise twice and the two sides could sum to two hundred and fifty-six —
the same trap the wipe records, avoided the same way, and asserted at every
pixel.

"Hard" means no feather rather than aliased: the coverage is still an exact
area, so a diagonal mask edge lands on partial values rather than on a
staircase.

### Convex, and what that costs

A concave outline is **refused**, not repaired. Taking its convex hull would be
a different shape, drawn by nobody, and impossible to notice until something
went to air. A concave mask is a union of convex ones and the union is not
built; the refusal says so.

A corner that does not turn at all — three points in a line — is allowed,
because it describes the same region and refusing it would refuse a rectangle
somebody built by dragging a fifth point onto a side.

**Softness is absent, and the reason is about cost rather than possibility** —
which is a distinction this project learned to make one milestone ago. A soft
polygon's ramp is a *minimum* of affine functions, and a minimum of affine
functions is not affine, so the centroid identity that makes a soft wipe exact
does not apply directly. It is still exactly computable: the region where each
edge is the nearest is itself convex, so the band partitions into pieces each
with an affine ramp. That is O(n²) clips per pixel, and it is deferred on that
number rather than on any claim that it cannot be done.

### What it cost the model

`Clip` and `Item` were `Copy`, and a mask owns a list of corners. They are
`Clone` now. Fourteen sites needed a `.clone()` or a borrow, all mechanical,
and the alternative — a fixed array of thirty-two corners on every clip,
masked or not — would have been a kilobyte per clip inside a sixty-four
kilobyte arena to preserve a trait nothing needed.

`SPRJ` goes to **version nine**: a clip writes a mask flag before its grade
flag, then the shape if there is one. The slate's file grew by exactly three
bytes, one per clip, and that was **measured against the previous commit in a
git worktree** rather than reasoned about: one clip grew by one byte, three by
three, seven by seven.

A file describing a shape the model would refuse is refused too, because the
reader goes through `Mask::new` like everything else. Reaching that check needs
a resealed file *and* the right bytes — a corner is two rationals, so an
eight-by-eight square's third corner is the words `8, 1, 8, 1`, and pulling it
in to `(3, 3)` is what turns a square into an arrowhead.

### And a reference capture, at last

`docs/VERIFICATION.md` has said since its first version that a reference frame
is kept beside its hash *so a failure can be looked at, not just counted*. The
writer landed several milestones ago and no reference was committed,
deliberately: the only frame the freestanding image composites is sixteen
pixels wide, and 16×9 is not a picture.

The host has no such bound. `tests/golden/reference.png` is three hundred and
twenty by a hundred and eighty: colour bars underneath, a ramp and a flat
colour above them meeting at a **soft wipe**, both inside a six-sided **mask**.
On a mismatch the test writes what it actually rendered next to the reference
and says where — which is the whole point of keeping a picture rather than a
hash. A digest says something changed; two files side by side say what.

The first version of it showed nothing, because both sides of the wipe rendered
the same test pattern and a wipe between two identical pictures is invisible.
That is the fixture lesson for the fifth time, so the capture now asserts that
the two sides differ and that a band of partial values lies between them.

**State: done.** Not done: soft masks, as above; concave masks, which need the
union; and a mask that moves, which needs the corners to be curves rather than
values.

## M8.6 — One asset per digest, and where it came from

*Requires nothing new.*

This milestone exists because a paragraph in the
[architecture](ARCHITECTURE.md) was read and checked. It said a media asset
carries a **location hint** and a **probed description**; the type had neither,
and the second one the *layering* forbids — `sapstudio-model` and
`sapstudio-media` are siblings, so an asset can never hold a
`FrameDescription`. The same block listed a transition as a kind of item, in a
document that explains at length why a transition is not one.

Chasing the missing hint found a real bug, and it is the more serious half of
this milestone.

### Two identifiers could name one digest

`Project::add_media` inserted whatever it was given, so a project could hold the
same content twice under two identifiers. That quietly **falsified the conform
round trip**: an export writes a source's digest, an import looks it up, and
with two candidates it finds the first — so a sequence cutting the same footage
under two identifiers came back pointing at one of them, with `LeftBehind`
reporting that nothing was lost.

The theorem was stated three milestones before the case that breaks it was
tried. It was not wrong about anything it tested; it was wrong about the world
it assumed.

The fix is what content addressing already meant: **one asset per digest.**
Adding the same content again gives back the identifier it already has. The
same bytes described two different ways — one length, then another — is a
contradiction and is refused, because a project whose record of an asset
depends on which order two files were opened in is a project that cannot be
reasoned about.

A file is different from the API here, and deliberately. `add_media` handing
back an existing identifier is right for a caller opening the same file twice
and *wrong* for a reader: a file listing one piece of content as two assets
would leave every clip indexing the second record pointing at the first, which
is a different programme arrived at silently. So the format refuses it by
name.

### A hint, and the word is load-bearing

The digest is what the media *is*; the location is only where somebody found
it. The model never resolves it, never opens it, and never compares two assets
by it — the same content found in a second place is the same content, and
adding it again does not rewrite the record, because moving a hint is its own
operation and doing it as a side effect of opening a file would edit a project
nobody asked to edit.

It is **bytes rather than text**. A path is whatever the platform says it is,
and Sapote's is not decided yet; refusing to interpret it is what keeps the
model free of the operating system, and it means a format that round-trips only
the paths it can read is not a format that loses somebody's media. A hint with
no bytes in it is refused, because a hint that says nothing looks like an
answer.

**Relinking is a hint moving, and nothing else.** Pointing a clip at different
bytes is different media and the digest says so, so there is no operation here
that swaps one piece of content for another while keeping its name — which is
the operation that silently changes what a programme is.

It is **not in the undo journal**, and that is a limitation rather than a
decision. The journal applies edits to a *sequence* and the media library
belongs to the project; making a media change undoable means the journal
becoming project-level, which is a larger change than a hint is worth. The
previous hint is returned instead, which is the whole of what undo would do.

`SPRJ` goes to **version ten**: every asset writes a length and then its hint,
four bytes when there is none. Measured against the previous commit rather than
reasoned about, again — one asset grew by four bytes, three by twelve, seven by
twenty-eight.

**State: done**, and **M8.7** makes offline media renderable. Not done: media
that is scanned or probed, which needs somewhere to scan.

## M8.7 — Offline media

*Requires nothing new.*

A project opens when the drive is not mounted. Until now a clip whose bytes
were missing failed the whole render, which is what makes an editor unusable on
the day it matters most.

### The fallback is in the planner, not in the graph

That is the only interesting decision here, and it is forced.

A source node's identity is a digest over the media, the tick and the
description — and **not** over whether the bytes happened to be reachable. So a
node that quietly fell back to a slate while evaluating would put that slate
into the cache under the real picture's key, and hand it back once the drive
came home. That is exactly the staleness a content-addressed cache exists to
prevent, met from the inside.

So `Library` gains one question — *can this be read at all* — asked **before**
a graph is built. An unavailable source is never named in the graph; the
planner emits a slate instead, and nothing is ever cached under a key that
lies. The test that pins it renders twice through **one pool**, absent and then
present, and requires the second to be the picture.

The trait's default answer is `true`. A library that cannot tell should try:
failing at `frame` is a worse answer than refusing to plan, and a default of
`false` would make every implementor that had not thought about it render
nothing at all.

### What offline looks like

Diagonal stripes, at an angle no camera produces. Not black, because that is
what an empty timeline shows; not a solid colour, because a programme might
legitimately contain one. Either would let "the drive is not mounted" look like
footage.

The stripe period is a **fraction of the frame** rather than a pixel count, and
that came from a test failing. A fixed sixteen-pixel period is a solid colour
on anything narrower than sixteen pixels — which is precisely the case where a
slate must not be mistaken for a shot of a red wall, and precisely the size the
freestanding image renders at. It is checked at five sizes from four pixels
across to full HD, and checked for running down *and* across, because a pattern
that varied along one axis only would be bars, which is a thing programmes
contain.

**State: done.** Not done: saying *which* media is missing, which is text on a
frame and text needs a font; and a per-clip record of the last time a source
was looked for, which is a cache rather than a project fact.

## M7 — Speed

*Requires `SAP-04`, then `SAP-10` and `SAP-11`.*

The first milestone where performance is the goal rather than a consequence.
SIMD becomes legal, sealed leaves under R-3.4 become worthwhile, the job graph
starts running on more than one core, and the first C or C++ codec libraries
become candidates.

**Done when:** every leaf is bit-exact with its Rust reference across the
committed corpus and a fuzz campaign; the parallel job graph produces
hash-identical output at every core count (R-6.2); and each leaf's benchmark
clears R-3.4.3's threshold.

## M8 — Suite

*Requires nothing new.*

The parts that make it a tool rather than a demonstration: titles and text,
transitions, keyframed effects, masks, media management, EDL export, OTIO
interchange, and the interface that makes all of it reachable.

**Done when:** an EDL exported from SapStudio and re-imported reproduces the
same cut exactly; the interface's layout is deterministic and its damage
tracking is proven bounded (R-10.4).

## M9 — One

*Requires everything above.*

The first release. A release means: reproducible artefacts, pinned digests, the
complete evidence set, the source of every vendored dependency, the licence
record, the QEMU transcripts, the golden hashes, and an honest statement of
what SapStudio is not — in Sapote's tradition of saying exactly where the
boundary is.

## Working in parallel

Two tracks run at once, and they only meet at the milestones that name a
capability:

- **The Sapote track** builds the capability ladder: `SAP-01`, `SAP-02`,
  `SAP-03`, `SAP-05`, `SAP-06`, `SAP-07`, then `SAP-08`, `SAP-13`, `SAP-04`,
  and finally `SAP-10` and `SAP-11`.
- **The SapStudio track** builds M2 first, because it needs nothing, then M3
  onwards as capabilities land.

M2 being independent of the platform is not an accident of planning. It is the
reason the language law puts Rust in charge of the model: the majority of this
application can be finished and proven before the operating system is ready to
run it.
