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
- **resampling**, which scaling a clip needs and which is where "looks about
  right" hides the most. It happens in **linear light**, because an average
  only means something over quantities that add, and on **premultiplied**
  samples, because averaging straight ones across an edge is the dark fringe
  round every badly keyed title. The forward map is inverted exactly — a
  rational two-by-two inverse is a determinant and four divisions — and a map
  with no inverse is refused. Area weighting is the exact overlap and is right
  for reduction; bilinear is right for enlargement; **choosing is the caller's**
  rather than a heuristic keyed on the scale factor, which would change a
  picture's look the moment somebody dragged past 100%. The general
  parallelogram path is checked against a product of one-dimensional overlaps
  it never forms;
- **transforms on clips**, which is that resampler given something to move. The
  linear part is dimensionless and the move is in fractions of the frame, so a
  project cut on a proxy and finished four times larger keeps the framing
  somebody chose. It acts about the frame's **centre**, because scaling about
  the corner sends a picture sliding off the bottom right the moment a slider
  moves. There is no rotation-in-degrees — four rationals instead, for the
  reason the wipe's direction is a vector — so a half, a third, a mirror and a
  quarter turn are all exact. A **mirror is not a refusal**: it has a negative
  determinant, and it is the *zero* one that has no inverse. And a transform
  that moves nothing goes through no resampler at all, which a test asserts by
  counting nodes rather than comparing pixels;
- **motion**, which is that framing given a curve. M4.6 named "a scale that
  pushes in" in its opening line and then deferred it, on the grounds that a
  curve on an item would need a keyframe name surviving a renumbering — and it
  does not, because the curve goes **on the clip**, where there is no index to
  survive. It is measured from the clip's own start, so moving a shot down the
  timeline moves its push-in with it rather than re-timing it. Which means a
  **cut re-bases the tail**: keyframes before the cut go negative rather than
  being dropped, because a curve holds its first value before its first
  keyframe and dropping that pair would flatten a move already underway into a
  hold. Join is the exact inverse, and refuses two halves whose animations do
  not line up. The renderer did not change by one line: the layer stack hands
  out a *resolved* transform, so nothing below it ever learns that anything
  moves — a claim with a test, and a third render beside it at a different
  framing, because the first two would agree just as well if the framing were
  being dropped on the floor;
- **rolling a cut and sliding an item**, the two trims a track could not do.
  A roll moves a cut without changing how long the programme is, so nothing
  after the cut moves and nothing after the cut has to be moved back; a slide
  moves an item without changing the item at all, its neighbours giving and
  taking to make room. **Both are their own inverses** with the sign turned
  round, which is why neither edit has to remember what it replaced. A
  dissolve&#39;s two conditions are about exactly what a trim changes — how long
  each side is, and how far into its media the incoming one starts — so a trim
  re-checks the dissolves it moved rather than trusting a check that ran when
  somebody drew them. And **removing an item was already a ripple delete**: a
  track stores no positions, so there is never a hole to close;
- **a face, written from scratch**, because one could not be taken from
  anywhere: every outline format worth reading is a parser and a hinting
  engine, every free face is somebody else's licence, and a bitmap would have
  to be drawn again at every size. A glyph here is a handful of **convex pieces
  that touch but never overlap**, so its coverage is their *sum* — exact, by
  the rasteriser wipes and masks already use, with no reasoning about
  antialiasing at all. Disjointness is measured rather than trusted: for every
  pair of pieces in every glyph, the exact area of their intersection, which
  must be nought. And `quantise` refuses a coverage above full, so an
  overlapping face is refused rather than drawn wrong. The face is therefore
  **the same shape at every size** — a glyph at twice the size covers exactly
  four times the area — with no hinting and no grid to snap to. Capitals,
  digits, lowercase and enough punctuation for a timecode, a digest and a
  name. Capitals were **one** measurement — cap line to baseline and nothing
  else — and lowercase needed three more: an x-height the bodies sit on,
  ascenders that reach the cap line, and descenders that hang below the
  baseline. Those four numbers are not a comment: a test measures every glyph
  against them, so a letter that drifted off its own line would fail rather
  than merely look wrong. A character it
  cannot set is **refused by name** rather than drawn as a box, because a
  slate that prints a message it was not given is the one thing a slate must
  not do. The whole repertoire is committed as
  [`tests/golden/specimen.png`](tests/golden/specimen.png) and compared byte
  for byte, because every other test would pass on a face whose letters were
  the wrong letters. And the face is a **table**, not a program that builds
  one: written as construction code it was the largest single item in the whole
  image at 23,807 bytes — a coordinate in a function body is an *instruction
  that stores a coordinate*, and there are some two thousand of them — so it
  moved into read-only data and gave **four pages** back. The specimen came out
  byte for byte identical, which is the proof the change was a change to how
  the face is written rather than to what it says;
- **and the offline slate says which media is missing**, which is the sentence
  that stood in this file's risk section for three milestones. The digest
  rather than a file name, because the digest is what the clip refers to: a
  name is a hint that may have moved. A legend carries **two** captions — the
  whole sentence and the part that matters — because a caption on a proxy has
  a real choice to make and neither answer is right at both sizes: at 320
  across it reads `MEDIA OFFLINE 4F3C9A21`, at 160 just the digest, and below
  that **nothing at all**, because a slate whose caption is a grey smear has
  told the viewer something false about how much it knows. The type is
  premultiplied **in light**, through the same conversion every other layer
  goes through; writing the coverage byte into the colour channels is the
  obvious way to build white type and is too dark along every edge by exactly
  the amount the transfer curve bends;
- **titles, which are media** — not a new kind of item and not a property of a
  clip, but an asset a clip cuts from like any other. That is the whole design:
  trimming, rolling, sliding, splitting, joining, dissolving, grading, masking,
  moving and animating a title all work already, and not one of them had to be
  told what a title is. A title is **named by what it says** — its digest is
  the digest of its own description — so the same card in two projects is one
  asset, two clips of it share a cached frame, and changing a word makes a
  *different* asset rather than quietly changing every clip of it. It has
  nowhere to be, so it cannot be relinked and does not need to be; and it is
  never offline, so the library is never even asked whether it has one. Its
  colour is three fractions of **full light** rather than three code values: a
  byte is a number in an encoding and the same byte is a different colour in
  sRGB than in a linear working space, so the ink means the same thing
  everywhere and the frame's own table spells it — 255 in full range, 235 in
  limited, and half of full light is 188 rather than 128. A card says as many
  lines as it
  needs, aligned left, centred or right, and the two questions stay apart:
  where the *block* goes is the card's own place, and the alignment is only how
  the lines sit against one another — so moving a left-aligned card does not
  re-align it. The lines stack at the face's **own** line spacing rather than
  at the em: this face descends, and lines an em apart would put every `g` in
  one line through every `A` in the next — where the two would sum past full
  coverage and the card would be *refused* rather than drawn heavy;
- **a fade on a clip**, which is the gesture a cut cannot make. A dissolve
  sits at a cut and needs two clips; the first item of a programme has nothing
  before it, so until this there was no way to bring a programme up from black
  at all. It rises from **nought** on the clip's own first frame and falls back
  to nought on its last — a different question from a dissolve, whose fraction
  never reaches either end because a frame there would repeat a neighbour. A
  fade from black *is* the black. Where the two fades of one clip meet, the
  smaller wins; where a clip's fade meets a dissolve at its cut, they multiply.
  A trim shorter than the fades on it is **refused** rather than silently
  re-timing somebody's fade;
- and **a bug that fade found**. Compositing a faded or masked layer scaled its
  premultiplied colour in *code values* — which this module's own header has
  forbidden since its first version: "a premultiplied sample is the encoding of
  light × coverage, not the encoded value scaled by coverage". A dissolve
  between two **identical** pictures sagged by twenty-eight code values in the
  middle. Every test the project had faded a layer that was **black**, where
  nought times anything is nought and the two arithmetics agree — the third
  time that blind spot has cost something here, and this time it had corrupted
  a test written specifically to pin the difference: the wipe's edge pixel
  asserted 154 with a comment saying the linear answer was the darker one, and
  both the number and the moral were the bug talking. It is 205, derived by
  hand, and `picture red` moved from 73 to 98;
- **retiming**: a clip plays its media at an exact rational speed. It keeps its
  length on the timeline; what changes is how much media it consumes to fill
  it. A clip at `24/25` is the standard pull-down and a clip at `0.96` is a
  rounding of it that drifts a frame every twenty-five seconds — slowly enough
  that nobody notices until a delivery. The **size** of the speed says how far
  and the **sign** says which way, so a reversed clip shows exactly the frames
  its forward twin shows; flooring `offset × speed` directly would round the
  other way and give `100, 99, 99, 98` against a forward `100, 100, 101, 101`.
  A reverse that would read before its media is refused when the speed is
  *set*, a speed of nought is refused as a freeze by another name, and sound is
  refused at any speed but real time until there is a resampler to pitch it;
- **a freeze**, which retiming named while refusing to be it: a speed of nought
  "would show one frame forever and consume no media — a freeze, which is a
  different edit with a different name". The second half of that sentence is
  the design. A freeze does *not* consume no media: it consumes exactly one
  frame, and `floor(offset × 0)` cannot say so — it puts the source end at the
  in point, claiming a clip that shows a frame reads none of it. So playback is
  two cases, at a speed or frozen, and a still's span is a single tick — which
  is what lets it be **held past the end of its own media**, as a still should
  be. Two stills join when they hold the *same* frame, because a still cut in
  two is two stills of one frame and join is the exact inverse of split; a
  still beside a moving clip does not join even where the arithmetic lines up.
  Sound is refused a freeze for a sharper reason than a speed: a held frame of
  sound is a held *block* of samples, which is a tone at the block rate;
- **a clip that animates itself**. A fade is the quick answer — two lengths and
  a straight ramp — and this is the general one: a curve on the clip with
  whatever shape somebody drew, a hold, a linear run, an ease. The two
  **multiply**, like everything else here that decides what is on screen.
  Measured from the clip's own start, so a ripple moves the animation with the
  shot, and a cut **re-bases** the tail rather than restarting it. An
  overshooting ease is clamped at the read, exactly as a track's automation is,
  because a layer past full coverage is a frame the compositor refuses. Sound
  is refused an opacity — not because sound cannot fade, but because its
  loudness is decibels and an opacity is a coverage. And it animates a **title**
  with no code of its own, because a title is media and a clip cuts from it
  like any other: a card that fades up and pushes in is a clip with a curve and
  a motion;
- and **a page count that was credited to the wrong thing**. That milestone
  only added — a field, an edit, two functions, a lane in the file — and the
  image *fell* two pages. Rather than write "it paid for itself", the claim was
  tested: on the previous commit, twenty-four bytes of dummy padding in `Clip`
  produce the same two pages and the same 3.4 KB off `Edit::apply`. The saving
  was bought by the clip crossing 320 bytes, past which the optimiser stops
  copying a clip inline into each of `apply`'s arms. **A struct getting bigger
  made the program smaller**, which is the opposite of what every earlier
  footprint note here assumed while reading a total;
- **markers**, which `ARCHITECTURE.md` has listed as planned since its first
  version. A note at an instant, with text — the one thing in this model that
  exists purely for the person editing: nothing renders it and no clip is
  affected by one. It does **not** move when an item ripples, because a note
  reading "the sync drifts here" is about a place on the timeline and an
  unrelated shot getting longer must not move it away from the thing it is
  about. That is a property of the *absence* of code, so the control **adds**
  the behaviour — a trim that slides every note — and the test that pins the
  decision fails. One per instant, because two at one instant is the same
  nothing as none: neither can be named. The text bound counts **characters**,
  and the fixture that proves it is the bound's worth of `é`, which is longer
  in bytes than in characters and is the only input that can tell the two rules
  apart;
- **a lift, which is the other delete**. Removing an item and closing the gap
  has always been here; taking the shot and *leaving* the hole never was, and
  half the deletes anybody performs are that one. The choice is not taste, it
  is about the rest of the programme: sound cut to picture stays in sync
  through a lift and slides through an extract, and an editor offering only one
  is making that decision for the user without saying so. It composes with the
  razor into the commonest gesture there is. Its inverse carries the shot back
  and refuses any slot that is not still the gap it left. And a dissolve on
  either boundary the item touches refuses the lift — *either boundary it
  touches*, not "at or after", because a lift renumbers nothing and the coarse
  check would refuse a lift at the head of a programme because of a dissolve at
  its end. The commit also retired an exemption **on schedule**: `Edit` carried
  a `clippy::large_enum_variant` `expect` arguing why *one* variant is bigger
  than the rest, a second variant now carries an item, the lint stopped firing,
  and the build said so — which is exactly what the paragraph beside it had
  predicted;
- **a razor, and the merge that undoes it**. Cutting one item on one track has
  been here since the model had items; what was missing is the gesture — a
  blade dragged **down** the timeline cuts every track it crosses, and dragged
  back it heals every cut it made. The difference is not convenience, it is
  **undo**: four splits are four entries in the history, and undoing once
  leaves three cuts behind. So a column is one edit whose inverse is one edit,
  over a set of tracks the edit itself carries — a `u128`, one bit per track,
  against a bound of 128 tracks and a compile-time assertion that the two
  numbers are one number. The set is *passed* rather than recomputed, which is
  what makes the inverse exact: a blade does not cut a track whose material has
  stopped or whose cut is already there, and a heal that recomputed could
  undo a cut nobody made. Both directions are two passes — work the whole
  answer out touching nothing, then publish it — so a refused razor leaves
  nothing behind. A control found that the *heal* had that reasoning and no
  test, which is the second half of an argument going untested because it felt
  like the safe half;
- **the point a framing acts about**. The centre was a good default and a poor
  rule — a lower third swings in on its left edge, and M8.24 made that sharper,
  because a turn about the centre was suddenly the only turn there was. The
  interesting part is what could *not* be done: acting about `a` rather than
  the centre contributes `(a − c) − M(a − c)`, which is a translation, so it
  looks foldable into the move the model already passes. That is true in
  **pixels** and false in **fractions**, which is the only space the model has
  — the vector to the anchor scales per axis and a rotation does not commute
  with that — so the folding is exact for a scale and wrong for every
  rotation, which is the case it was added for. There is a test that folds it
  and compares: on an 8×4 picture a diagonal map agrees byte for byte and a
  three-four-five turn does not. And a control here found a real gap: the
  anchor was in the render node's cache key and nothing had ever asked it to
  be, so two pivots differing only *down* the frame collided;
- **a turn, and it is exact**. This model said since M8.9 that it has no
  rotation, because "a sine and a cosine are not exact, and a project whose
  framing depended on them would drift". True of an *angle*; false of a
  **rotation**. Put `t = tan(θ/2)` and `cos = (1 − t²)/(1 + t²)`,
  `sin = 2t/(1 + t²)` is a rational pair for every rational `t`, with
  `cos² + sin² = 1` exactly and a determinant of exactly one — and the rational
  points on the circle are *dense*, so a quarter turn, a three-four-five, and
  everything between them are all available with nothing approximated. Turns
  compose without renormalising, so a thousand of them is still exactly a turn;
  four quarter turns are the identity on the nose; and a picture turned four
  times through the resampler comes back **byte for byte**. The type stores the
  *point*, because `t` reaches every rotation except the half turn, which sits
  at infinity; the *curve* stores `t`, because a curve needs somewhere
  unbounded to live. One lane turns a mask about its own centroid and a framing
  on the left of its base transform — `R·M`, not `M·R`, which differ exactly
  when the framing mirrors. The renderer did not change by a line, and the
  image did not move by a page. And testing the resampler's own long-standing
  claim that it works "however the picture is turned" found the sharpest
  fixture lesson yet: a **quarter turn cannot test rotation**, because a right
  angle sends the pixel grid onto itself and leaves every preimage
  axis-aligned. The mutation that proves it fails one test in 235, and it is
  neither quarter-turn test;
- **a grade that comes on over a shot** — the last place a parameter was a
  value where it could be a curve, which is the phrase the two milestones
  before it both ended on. Not *which* look: a digest is not a quantity and two
  tables have nothing between them to interpolate. What animates is the
  **strength**, nought for the clip untouched and one for the look applied
  exactly as it always was — and one is what an absent curve reads, which is
  why every project written before this keeps its looks. The mix happens in the
  table's own **code values**, `c + s·(f(c) − c)`, for the reason that module
  has given since its first version: apply an operation in the space its
  definition is written in. That is the **opposite direction** from the
  compositor, which mixes only in light, and both follow from the one rule. It
  is a testable difference rather than a stated one: half a look taking a
  mid-grey to black lands on 64, and the control that moves the mix into light
  lands on 92.374 — both derived by hand from the sRGB curve before either was
  run. And the commit corrected two sentences it had written itself: that the
  interpolation's arrangement is what makes a full-strength grade exact (it is
  the multiply, and both arrangements are exact at the ends), and that the
  format's field order is what lets a file's refusal see the grade (it is the
  builder chain, and swapping both halves of the format breaks nothing). Both
  were found by controls — one by specifying it, one by running it;
- **a mask that animates** — an iris that opens, a vignette that breathes, a
  shape that sweeps a card on. A uniform scale and a move, not the corners: a
  corner that moved on its own could turn a convex outline **concave** part way
  through, and this build computes an exact area only for a convex one, so
  per-corner animation would mean a refusal arriving at a *frame* rather than
  at the edit. It scales about the mask's **own area centroid** — a trapezoid's
  corners average to `(1/2, 1/2)` and its area balances at `(1/2, 4/9)`, and
  scaling about the wrong one drifts a shape sideways while it grows. Which
  gives a **text reveal** out of the two lanes already there: a strip scaled by
  `s` and moved right by `(s − 1)/8` keeps its left edge at nought and sweeps a
  card on. And the same milestone put the two pages back — `Clip` 344 → 416,
  the image 91 → 93 — so the threshold above is one effect at one size, not a
  trend to lean on;
- **a title's colour, named in light**. Titles shipped white with an argument
  that was right — three bytes in a model that has never held a colour would be
  three bytes in *which encoding* — and a conclusion that was not. The way out
  is to store fractions of **full light** rather than bytes: the same ink is
  255 in a full-range frame and 235 in a limited-range one, and half of full
  light is 188 rather than 128, because sRGB bends. Each of those numbers is
  derived from the definition in the test that asserts it. And it found a real
  refusal: a slate caption is antialiased, so packing a hard 255 made every
  partly covered pixel claim more light than its coverage allowed, and a
  limited-range slate failed with `NotPremultiplied` rather than drawing
  something slightly wrong. A card, whose stencil has no soft edge, was never
  affected — which the tests now say, one each, rather than one claim covering
  both;
- **the program renders**, on the freestanding target, and says what came out.
  The slate composites two layers through a fade at frame 12 of a 24-frame rise
  and prints the SHA-256 of the result — a golden render hash over the layer
  stack, the plan, the graph, the compositor and the pool. `picture red` is 98,
  and every step of that is derived by hand: fading a premultiplied layer
  scales its coverage too, and `over` works in linear light. Until this
  existed, `sapstudio-render` had **no symbol in the image at all** — half the
  project was absent from every footprint recorded, and linking it cost
  seventeen pages;
- a freestanding program image that links at Sapote's user base as a static,
  non-PIE `ET_EXEC` with no dynamic section, no relocations, and no SIMD, built
  twice into different directories and compared byte for byte.

1066 tests, no third-party dependencies, no `unsafe` outside the two crates
that are allowed it, and every rule this project wrote down is enforced by
something that runs. 494 invariants have been checked by
deliberately breaking the code and requiring the break to be caught; four of
those found real bugs, nine found gaps in the tests themselves, two found a
sentence claiming more than the code delivers, and one
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
