<!-- SPDX-License-Identifier: GPL-3.0-only -->

<h1 align="center">SapStudio</h1>

<p align="center">
  <strong>A professional non-linear video editor, native to Sapote.</strong><br>
  Not a port, not a portable application with a Sapote build — an editor that
  owns its operating system.
</p>

<p align="center">
  <a href="LICENSE"><img
    src="https://img.shields.io/badge/license-GPL--3.0--only-595976"
    alt="GPL-3.0-only"></a>
  <img
    src="https://img.shields.io/badge/status-foundation-18181C"
    alt="foundation stage">
</p>

## Status

Foundation stage. The parts of an editor that need no operating system are
being built and proven now; the parts that need one are waiting on Sapote.

[Sapote](https://github.com/saudaljuaid/Sapote) v1.1.0 cannot yet run
SapStudio. It has no native application ABI, no writable filesystem, no
userspace memory service, no audio device, no second core, and no architectural
guarantee that a Ring 3 program may execute a single SSE instruction. Its only
userspace surface is a bounded Linux compatibility boundary that admits three
checksum-pinned BusyBox programs.

**What exists and is proven, today:**

- exact rational time, timebases, half-open ranges, and drop-frame timecode,
  swept over every frame of a whole day at eight rates — twenty-nine million
  round trips, each of which must name its own frame back;
- the project model — sequences, tracks, clips, gaps, and eight edit operations
  — where overlapping items are unrepresentable rather than merely rejected;
- undo and redo as an algebra, checked over two thousand generated editing
  sessions, with the check itself shown capable of failing;
- a project file that is versioned, length-prefixed and digested, so that
  **every single-byte change to a valid file is refused** — swept, not claimed
  — with SHA-256 written here and checked against the published vectors;
- a save that cannot lose the last one: encode, write scratch, read it back and
  compare, then commit. Each of its four failure modes is a test that requires
  the previous file to survive whole;
- frames that **cannot be built without a complete colour description** — no
  `Unknown` primary, no `Unspecified` transfer, no `Default` anywhere, so the
  untagged frame behind every washed-out export is not a value this program can
  construct — with plane arithmetic checked at every dimension in a range, and
  a frame with an alpha channel required to say whether it is straight or
  premultiplied, because that is the difference between a clean edge and a
  dark fringe;
- a bounded, content-keyed frame pool that evicts by use, deterministically,
  and refuses to let one key ever name two different frames;
- `SPRW`, an uncompressed mezzanine format whose byte sweep found a real
  integrity gap before it shipped, and closed it;
- a colour derivation computed in **exact rationals rather than floating
  point**, so a gamut converted to itself is the identity matrix and every row
  of a conversion sums to exactly one — with the derived BT.709 coefficients
  and the derived BT.709-to-BT.2020 matrix checked against the numbers the
  standards print;
- transfer functions — sRGB, BT.709, BT.2020, both pure gammas, ST 2084 and
  hybrid log-gamma — computed with **integer arithmetic and no libm**, because
  `pow` is not specified bit-for-bit by IEEE 754 and two machines with
  different maths libraries would otherwise export different pixels;
- sine and cosine measured in **turns rather than radians**, so reducing an
  angle to one revolution is masking bits and loses nothing — a quarter turn's
  sine is exactly one, and an angle ten thousand revolutions out gives the same
  answer bit for bit as the same angle at the origin, which no floating-point
  library can promise;
- histogram, waveform and vectorscope, which are counts rather than pictures,
  checked against expectations worked out by counting rather than by running
  the code — and on the vectorscope, against two properties that are exact:
  every grey sits precisely on the origin under every matrix, and full red sits
  at exactly +1/2 red-difference in BT.601, BT.709 and BT.2020 alike, because
  the coefficient cancels itself out of the definition;
- frame conversion in the order colour science requires — gamut changes happen
  **in linear light and nowhere else** — with a scaler and a chroma filter
  refused rather than guessed, because each is a decision with a name;
- a compositor that works **in linear light and nowhere else**, checked as the
  algebra `over` actually is — a transparent layer leaves the one beneath it
  bit-for-bit unchanged, an opaque layer hides it entirely, and the two ways of
  grouping three layers agree to within a single code value, so grouping clips
  into a compound cannot change the picture — with one pixel computed by hand
  to pin it: full white at half coverage over mid-grey is 205, where a
  compositor that adds code values gives 252;
- and a bug that compositor found in code that was already passing its tests:
  a colour conversion was writing 255 for every alpha byte, so any keyed frame
  that crossed a colour space came out a **solid rectangle**. The test that
  should have caught it converted opaque bars, which have nothing to lose;
- **a sequence rendered at an instant** — the spine of an editor, and the piece
  that makes everything above it do something. Which clip is on each track,
  which frame of it the playhead wants, and the layers composited bottom first
  onto opaque black. Three decisions are named rather than left implicit: higher
  tracks are on top, **a gap is transparent rather than black** so an upper
  track with sparse material does not blank out everything beneath it, and a
  track that has stopped is not a track full of black past its end;
- **dissolves**, which need no operator of their own: the model reports both
  sides of the cut, the outgoing at full opacity and the incoming at an exact
  fraction, so `over` computes the cross-fade — in linear light, so a
  white-to-black dissolve steps 231, 203, 170, 124 rather than the evenly
  spaced numbers a code-value fade would give, each worked out by hand;
- **its sound mixed over a span**, each track at its own fader — set by an
  edit, undoable, and saved, because a mix level that lived only in a function
  call would be a mix nobody could deliver — and where the interesting
  arithmetic lives: a
  frame at 29.97 is 1601.6 samples, so no frame holds a whole number of them
  and none ever will. A frame's samples are bounded rather than counted — each
  block is 1601 or 1602, each block's end is the next one's beginning, and over
  three hundred frames they sum to exactly 480,480, a number arrived at by
  arithmetic rather than by running the code;
- a render graph in which **a cycle is unrepresentable** and every node is
  identified by a digest over its inputs, evaluated in every order a scheduler
  could choose and checked to give the same answers — proven now, while it is
  nearly trivial, so it still holds when there is a second core. **The timeline
  renders through it**, so a pool kept between renders means scrubbing back
  over a cut decodes nothing again, and two sequences cutting the same footage
  share one cached frame — because the graph names media by what it *is*
  rather than by any project's index for it;
- **loudness as ITU-R BS.1770 defines it** — the measurement a delivery is
  actually judged against, rather than the peak meter that says almost nothing
  about it — checked against EBU Tech 3341's own compliance cases, which are
  *generated* rather than shipped: a −23 dBFS tone reads −23.0 LUFS and a −33
  one reads −33.0, both inside the tenth of a unit the standard allows;
- **a mixer's arithmetic**, computed with integers because Sapote preserves no
  floating-point state and because `pow` is not specified bit-for-bit anyway:
  zero decibels is **exactly** one, twenty decibels is **exactly** ten, and six
  decibels is asserted *not* to be a doubling — two full-scale sources trimmed
  six decibels each still clip, which is what the 6 versus 6.020599913
  difference actually costs. A constant-power pan holds `left² + right²` at one
  across the image, a signal against its own inverse nulls to precisely zero,
  and full scale is **reported rather than reached quietly**;
- **parameters that change over time** — the keyframes and Bézier eases every
  editor draws as two handles — held past their ends rather than extrapolated,
  because continuing the slope is how a parameter set to reach 100% at the end
  of a shot arrives at 340% two shots later. Linear is *exact*: a twenty-four
  frame ramp is `n/24` at frame `n`, thirds and sevenths included. The ease is
  where the honesty is: solving `x(t) = time` on a cubic needs a cube root,
  which is not rational, so there is no exact answer to find — the parameter is
  bisected to one part in a million, evaluated in 128-bit integers, and rounded
  once. The size of the approximation is stated and asserted rather than left to
  a floating-point library to decide differently on each machine. The first
  parameter to read one is a picture track's opacity, which *multiplies* what
  the clips on it are doing rather than replacing it — two things decide what is
  on screen during a dissolve inside a fade, and either alone throws the other
  away;
- **a fader that moves while the sound is playing.** A gain applied one frame at
  a time and held flat puts a step at every frame boundary — a buzz at the frame
  rate on any fast move. So the mixer takes a *ramp*, whose interval is half
  open: it starts at one gain and arrives at the next at the sample *after* its
  last, which is the next block's first. That is what makes consecutive blocks
  tile a fader move rather than repeat a value at every seam — and a repetition
  at a regular interval is a tone. Mute is not a fader position but a switch, so
  a muted track stays muted whatever is drawn on it;
- **the waveform a timeline draws sound from**, summarised once into a pyramid
  of blocks rather than re-reduced on every scroll — the lowest sample, the
  highest, and the mean square. Two numbers rather than one magnitude, because
  brass and speech and a kick drum genuinely lean one way and a mirrored
  drawing is a picture of a signal nobody recorded. The highest of two highests
  is the highest, so **one sample at the rails in half a second of silence is
  visible at every zoom** — a click cannot hide when you zoom out, and cannot
  be invented either. The one number that is not exact, the mean square, folds
  upward in 128-bit sums and divides once, so it is within *one* however far
  out you go rather than one per level. It stores as `SPPK`, whose header holds
  the digest of the sound it summarises — so a stale peak file is something you
  can *see*, rather than infer from a modification time that a copy, a restore
  or a clock change will happily lie about;
- **three-dimensional lookup tables**, the form every grade travels in —
  interpolated *tetrahedrally*, which is not a preference but a measured
  difference: on the neutral axis the four terms telescope to a straight run
  between the cell's diagonal corners, so **a grey stays grey exactly**, while
  trilinear mixes all eight corners and tints twenty-nine greys out of
  thirty-nine on the same table. Both are implemented, and trilinear is there
  to be failed by that test — a design decision with no test showing what the
  rejected option does is a preference rather than a decision. They arrive as
  `.cube` files, whose decimal text is read *exactly* — `0.123456` is
  `123456/1000000` — because going through a binary float would throw that away
  on the way in, in a project with no floating point anywhere else. Applied in
  the encoding the table was *authored for* — declared, not inferred, and a
  frame in another encoding is refused — and deliberately **not** in linear
  light, which is the opposite of the compositor and right for the same reason:
  apply an operation in the space its definition is written in;
- **CMX 3600 edit decision lists**, read and written — the interchange format
  every other system still speaks — with each of its four traps named by a test
  that fails when the trap is sprung: the exclusive out point, the stateful
  `FCM` line, the two disagreeing statements of drop-frame, and the eight
  characters a reel name gets. Every prefix of a valid event line is swept and
  must be refused;
- **a sequence conforms to one of those lists and comes back**, and the claim
  is a theorem rather than a hope: if the export leaves nothing behind, writing
  the list, parsing it and importing the result produces a sequence **equal** to
  the one that went in — by `PartialEq` on the whole value, so a field nobody
  thought to compare is compared anyway. What it cannot carry is counted and
  reported rather than dropped quietly, and the line between reporting and
  refusing is drawn where the frames are: a grade or a fader leaves a cut that
  is still correct and only looks wrong, while a second picture track written
  to the format's one video channel would be a different programme. Reel names
  are the source digest's first eight characters, with the whole of it in the
  comment, so two sources that would share one are refused rather than written.
  The importer read its frame numbers a quarter too fast until a test went
  through the *text* — the parser labels everything at thirty because the file
  cannot say, and a round trip that skips the file compares a value with
  itself;
- **shapes, rasterised by exact area** — the coverage of a pixel is the area of
  it that lies inside the shape, computed as a rational and quantised once,
  rather than sampled at the centre or at sixteen sub-positions. That is what
  keeps a wipe's edge sliding rather than crawling: the area under a line moves
  continuously as the line does and a sample does not. Checked three
  independent ways — a closed form the rasteriser never calls, agreeing pixel
  for pixel at six orientations; a rectangle's coverage against the product of
  its two overlaps; and the strongest, a relationship rather than a bound, that
  **a shape's coverage summed over the picture is the exact area it encloses**.
  A wipe then needs no compositing operator of its own, for the reason a
  dissolve does not: mask the incoming layer with the plane and put it `over`
  the outgoing one;
- **wipes, which are dissolves that spend their fraction differently** — the
  two are timed identically at the same cut, and a test compares their layer
  stacks frame by frame across a whole programme and requires them to agree
  about everything except what the fraction is *for*. A dissolve spends it on
  the incoming layer's opacity; a wipe carries it, and both clips stay whole
  because the incoming one is not half-faded but entirely there behind an edge.
  The direction is a **rational vector rather than an angle**, so straight
  across is `(1, 0)` and a true diagonal is `(1, 1)` rather than a rounding of
  forty-five degrees — an angle would need a sine and a cosine and neither is
  exact. Its length carries no meaning, which is a test, because a
  normalisation creeping in would need a square root. Their edges are hard or
  **soft**, and a soft one is exact too: the integral of an affine ramp over a
  polygon is its area times its value at the polygon's centroid, which is the
  definition of a centroid rather than a result about it — so a feathered edge
  is two clips and a moment rather than the "much larger case analysis" this
  project had written down and believed. Softness is a fraction of the wipe's
  travel, so it means the same thing at every size and angle;
- **masks on clips**, the same coverage machinery pointed at a clip instead of
  a transition. Corners are fractions of the frame, so a mask drawn on a proxy
  is the same mask on the finish. The winding is *measured* from the polygon's
  own area rather than demanded of the caller — getting it wrong inverts the
  mask, which is the most confusing failure a mask has — and an inversion flips
  the **byte** rather than the shape, so the two sides sum to exactly full
  coverage at every pixel. A concave outline is refused rather than quietly
  replaced by its convex hull, which would be a different shape drawn by
  nobody;
- **a reference capture that is a picture** — at
  [`tests/golden/reference.png`](tests/golden/reference.png), which is
  colour bars under a ramp and a flat colour meeting at a soft wipe, both
  inside a six-sided mask. On a mismatch the test writes what it actually
  rendered beside the reference and names both paths, because a hash says
  something changed and two files say what;
- **one asset per digest**, which is what content addressing already meant and
  what a stated theorem quietly depended on. Two identifiers naming one digest
  falsified conform's round trip — the export reported nothing lost and the
  import resolved both clips to whichever it found first. Adding the same
  content again now gives back the identifier it already has; the same bytes
  described two different ways is refused. Assets carry a **location hint**,
  which is bytes rather than text because a path is whatever the platform says
  it is, and relinking is that hint moving and nothing else — pointing a clip
  at different bytes is different media, and the digest says so;
- **a project that opens when the drive is not mounted** — a clip whose media
  is missing renders an offline slate rather than failing the whole frame. The
  fallback is in the *planner*, not the graph, and that is forced: a source
  node's identity covers the media, the tick and the description and **not**
  whether the bytes were reachable, so a node that fell back while evaluating
  would cache the slate under the real picture's key and serve it after the
  drive came home. The test renders twice through one pool. The slate is
  diagonal stripes whose period is a *fraction of the frame* rather than a
  pixel count, because a fixed period is a solid colour on a small frame, which
  is exactly where a slate must not look like footage;
- **the program renders**, on the freestanding target, and says what came out.
  The slate composites two layers through a fade at frame 12 of a 24-frame rise
  and prints the SHA-256 of the result — a golden render hash over the layer
  stack, the plan, the graph, the compositor and the pool. `picture red` is 73,
  and every step of that is derived by hand: fading a premultiplied layer
  scales its coverage too, and `over` works in linear light. Until this
  existed, `sapstudio-render` had **no symbol in the image at all** — half the
  project was absent from every footprint recorded, and linking it cost
  seventeen pages;
- a freestanding program image that links at Sapote's user base as a static,
  non-PIE `ET_EXEC` with no dynamic section, no relocations, and no SIMD, built
  twice into different directories and compared byte for byte.

699 tests, no third-party dependencies, no `unsafe` outside the two crates that
are allowed it, and every rule this project wrote down is enforced by something
that runs. 193 invariants have been checked by
deliberately breaking the code and requiring the break to be caught; three of
those found real bugs, five found gaps in the tests themselves, and one
named the exact cost of a shortcut that had looked harmless. They
are listed, with what each refusal looked like, in
[Verification](docs/VERIFICATION.md).

**What it is waiting for:** `SAP-01` through `SAP-08` in
[the platform contract](docs/PLATFORM_CONTRACT.md) — the capabilities Sapote
must grow, each written as a measured profile in Sapote's own vocabulary.

## The shape

| Concern | Language |
| --- | --- |
| Project model, timeline, file management, undo/redo, media pipeline coordination, UI state | Rust |
| The single boundary to Sapote's userspace ABI, and later to external codec libraries | C ABI |
| Tiny freestanding shims where the boundary is instruction- or register-shaped | C |
| Sealed inner loops, only after a correct Rust implementation exists to measure against and stay bit-exact with | C++ |

Two crates may contain `unsafe`. Every other crate forbids it by attribute.

## Documents

| Document | Contents |
| --- | --- |
| [Charter](docs/CHARTER.md) | What SapStudio is, what it refuses to be, and why it is Sapote-only |
| [Engineering rules](docs/ENGINEERING_RULES.md) | The normative rules |
| [Open-source map](docs/DEPENDENCIES.md) | Every component considered, with licence and verdict |
| [Dependency policy](docs/DEPENDENCY_POLICY.md) | How a dependency enters the tree, and how it leaves |
| [Platform contract](docs/PLATFORM_CONTRACT.md) | What Sapote must provide, and what already works |
| [Architecture](docs/ARCHITECTURE.md) | The planned crate map and data model |
| [Roadmap](docs/ROADMAP.md) | Milestones, smallest first |
| [Verification](docs/VERIFICATION.md) | What counts as evidence |
| [Brand](docs/BRAND.md) | The mark, the palette, the naming, the voice |
| [Glossary](docs/GLOSSARY.md) | Editing vocabulary, defined exactly enough to implement |

## Checks

```sh
make hooks          # enable the repository's pre-commit check
make lint           # hygiene: whitespace, headers, links, width; the crate
                    # layering, against the block in docs/ARCHITECTURE.md that
                    # declares it; and every test and control count the
                    # documents assert, against the tree
make check          # rustfmt and clippy::pedantic, warnings denied
make test           # the host suite
make image          # the freestanding program image for Sapote
make audit          # R-13.4 and R-13.6 on the linked ELF, its control, and
                    # a breakdown of where the image's pages actually go
make reproducible   # two clean builds, compared byte for byte
make verify         # all of the above
```

The gates that need an emulator arrive with the capabilities that make them
possible; they are specified in [Verification](docs/VERIFICATION.md) so that no
milestone can quietly land without them.

## Licence

[GPL-3.0-only](LICENSE), the same licence as Sapote. Every dependency must be
compatible with it, verified from vendored source rather than from registry
metadata.
