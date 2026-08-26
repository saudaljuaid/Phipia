<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

SapStudio takes Sapote's position on evidence without softening it: a green run
proves the checked contract under its recorded conditions, and nothing more. It
does not imply an untested format works, an untested machine boots, or a
performance number holds anywhere else.

## Gates

The commands below are the whole contract. Each milestone adds targets; none
removes one.

```sh
make lint           # repository hygiene: whitespace, tabs, headers, links, width
make check          # rustfmt and clippy::pedantic, warnings denied
make test           # host tests: unit, property, golden, and boundary
make image          # the freestanding program image
make audit          # R-13.4 and R-13.6 on the linked ELF, and its control
make reproducible   # two clean builds into different directories, compared
make verify         # all of the above
```

Two more arrive with what makes them possible: `make qemu-tests` with `SAP-01`,
and a dependency gate with the first dependency.

Fuzzing has arrived early, in the form the rules require rather than the tool
they name. `cargo-fuzz` needs a network to vendor; until the gate can run, the
project file's decoder is swept deterministically instead — every byte of a
real file changed to five values, every prefix, every extension, and several
hundred thousand bytes of seeded garbage, all in `make test`. When
`cargo-fuzz` arrives it inherits the corpus; it does not start one.

`make verify` is the one that has to be believed, so it is the strictest. It
rejects: any compiler warning; any `clippy::pedantic` finding; unformatted
code; an undefined symbol; a relocation record; an unexpected linker section; a
global offset table past its bound or pointing outside the image; a PIE or
dynamic image; a W+X segment; an executable stack; an image based anywhere but
the agreed address; any floating-point, MMX, SSE, or AVX instruction while
`SAP-04` is outstanding; and two clean builds that differ by a single byte.

It also runs `tools/audit-control.py`, which mutates a copy of the image into a
position-independent one and into one with an SSE instruction in its text, and
requires the audit to refuse both by name. The audit is therefore shown capable
of failing on every run rather than on the day someone remembers to check.

## What counts as evidence

**Acceptance is a QEMU scenario on Sapote.** Host tests are supporting
evidence. A behaviour that has only ever been observed on the host is not a
behaviour SapStudio claims — which is why nothing in this repository claims the
program runs. It claims the image has a shape Sapote accepts, which is a
different and much smaller statement, and it is the one the audit proves.

**A scenario is bounded and named.** Each has a stable identifier, a required
serial transcript, an expected guest exit value, and a fixture built by a
committed deterministic tool. Fixtures are ordinary local files attached
read-only to emulated devices; host-device passthrough is never used as
evidence, exactly as Sapote requires.

**A screenshot shows presentation, not correctness.** Pixel comparisons are
used for what they can prove — that the drawn output has not changed — and a
one-pixel mutation must fail the comparison.

**A performance claim is a committed benchmark plus a machine profile.** A
number without both is an anecdote.

## Negative controls

Every invariant carries one, and the procedure is Sapote's: make one isolated
temporary mutation that violates the invariant, run the narrowest relevant
gate, observe the refusal by its name, and restore the source. The mutation is
never committed; the fact that it was performed, and the exact refusal it
produced, belongs in the pull request.

Examples this project will need:

| Invariant | Mutation | Expected refusal |
| --- | --- | --- |
| Undo restores exactly (R-9.2) | Drop one field from an edit's inverse | Property test fails on a specific generated sequence |
| Saves are atomic (R-9.4) | Interrupt the write after the first block | Previous file intact, named error, no partial file |
| Renders are deterministic (R-4.1) | Reorder one reduction | Golden hash mismatch |
| Frames are fully described (R-8.2) | Drop the transfer function from one frame | Named refusal before any conversion |
| Caches cannot go stale (R-8.5) | Omit the parameter set from the key | Test observes a wrong-parameter hit |
| The image shape is conforming (R-13.4) | Remove `-no-pie` | ELF audit fails on `Type: DYN` |
| Resources are released (R-5.6) | Skip one pool return | Census mismatch at teardown |
| A parser is bounded (R-11.2) | Raise one limit | Fuzz target finds the allocation immediately |
| A save verifies itself (R-9.4) | Drop the read-back comparison | The corrupting-storage test fails, and only that one |
| A file is digested (R-9.3) | Skip the digest check | The single-byte mutation sweep accepts a changed file |
| A reel's description is covered (R-8.2) | Digest only the samples | The sweep accepts a changed transfer function, which silently changes every frame |
| A transfer curve is the standard's (R-8.3) | Move sRGB's breakpoint by a factor of ten | Three tests fail by name: the reference values, the round trip, and monotonicity |
| Luma weights sum to the scale | Round green independently instead of taking the remainder | BT.2020's weights no longer sum, so a white field would not measure as white |
| A gamut conversion happens (R-8.3) | Skip the matrix | The saturated-colour test fails, and **only** that one |
| A node's identity covers its inputs (R-8.5) | Leave the input out of the digest | Two tests fail: the identity itself, and order-independence, because a shared cache then returns the wrong frame |
| Alpha association is stated (R-8.2) | Drop the check that a format with alpha must name one | Three tests fail: both description tests and the forged-file test, because an untagged frame becomes constructible |
| Alpha association is part of identity (R-8.5) | Leave the alpha tag out of the frame digest | Straight and premultiplied frames collide in the pool, and the golden digests move |
| A mezzanine carries the association (R-8.2) | Write a constant alpha tag | Nine tests fail, because every reel then describes something it does not hold |
| Compositing happens in light (R-8.3) | Add code values instead of decoding first | Four tests fail, including the hand-computed pixel, which lands 47 code values too bright |
| `over` trusts nothing (R-8.2) | Drop the premultiplied check from `over` | The dark-fringe test fails: relabelled straight samples are composited |
| A conversion carries coverage (R-8.2) | Write a constant alpha byte again | Three tests fail, one of them the round trip that used to assert the bug |
| A scope derives its matrix (R-8.3) | Use a fixed BT.601 matrix whatever the frame says | The green-box test fails and the axis test does **not** — the axes are matrix-independent by construction, which is why the green box is also asserted |
| A blank frame is black (R-8.2) | Fill it with zeroes again | Two tests fail: zero is below the legal floor of a limited-range luma plane, and is saturated blue-green in a chroma one |
| An EDL out point is exclusive (R-11.3) | Add one to the span | Two tests fail; without this every clip in every imported file is one frame long |
| `FCM` is stateful (R-11.3) | Read only the first one | Two tests fail, both about a file that changes mode halfway |
| Two statements of drop-frame must agree (R-1.3) | Trust the punctuation and ignore the `FCM` line | The conflict test fails: a contradictory file is read rather than refused |
| Keyword matching is not suffix matching | Match `DROP FRAME` before `NON-DROP FRAME` | Twelve tests fail, because every non-drop file is read as drop-frame |
| The pan law keeps power constant | Send the fraction itself rather than its square root | Three tests fail, including the centre, which lands 3 dB out |
| Clipping is reported (R-7.2) | Clip the sample but not the report | Two tests fail; the mix is unchanged and the mixer has stopped saying so |
| A sum rounds away from zero | Truncate towards zero instead | The half-scale mix test fails: every quiet passage drifts towards silence |
| Unity is exact — **control passed, claim was wrong** | Remove the zero-decibel fast path | *Nothing failed.* `pow` computes `exp2(y·log2(x))`, and at `y = 0` that is exactly one, so the shortcut was never load-bearing. The comment claiming it was is corrected, and a test now checks the general path reaches unity on its own |
| Higher tracks are on top | Composite the stack top first | Three tests fail, including the hand-computed half-covered pixel |
| A gap is transparent (R-9.1) | Let a gap stand in for its track's first clip | Two tests fail: an upper track with sparse material blanks out everything beneath it |
| A playhead reads the right source frame | Drop the clip's source start from the sum | Two tests fail; without them the whole clip plays the wrong material, not one frame of it |
| A sample position is the floor | Round to nearest instead | **Only one** test fails — the one asserting the definition. The tiling test passes either way, because rounding is monotone too, and the comment claiming tiling was the reason for the floor is corrected |
| A frame's sample count is not constant | Divide the span by the frame count | Two tests fail: at 29.97 the blocks stop tiling and stop being contiguous |
| A fader reaches the mix | Mix every track at unity | Three tests fail, including the muted track, which is heard |
| A fader survives a save (R-9.3) | Write unity for every track | Two tests fail; a delivered mix would arrive flat |
| An edit's inverse is where it was (R-9.2) | Return unity as the inverse | Undo moves the fader to a default rather than back |
| A faded layer stays premultiplied (R-8.2) | Scale the coverage and leave the colour | The opaque-dissolve test fails — and the white-to-black one does *not*, because black has no colour to scale. Two tests, two mutations, neither redundant |
| A dissolve is a real mix at every frame | Let the fraction reach nought and one | Two tests fail: the first and last frames of every dissolve repeat their neighbours |
| A dissolve uses handles (R-9.1) | Clamp each side to its own in and out points | The handle test fails; both ends of every dissolve would freeze on a frame |
| Every input is checked (R-6.1) — **control passed, gap was real** | Report only one of `Over`'s two inputs | *Nothing failed.* The identity computation reaches both inputs and refuses first, so two mechanisms enforce one rule — and the rule was resting on the one nobody had tested. A test now names both sides, and the same mutation fails it |
| A cache key covers the frame wanted (R-8.5) | Leave the tick out of a source node's identity | The caching test fails: every frame of a clip returns the first one |
| Media is named by content (R-8.5) | Name it by the project's track index instead | Eight tests fail, because every layer fetches the wrong footage |
| Opacity is part of identity (R-8.5) | Leave it out of a fade's identity | A dissolve caches its first frame and shows it for the whole transition |
| The octant table is a table | Replace it with a parity trick | Six tests fail, starting with a right angle's sine at nought — which is how the table came to be written out in the first place |
| An angle reduces by masking | Reduce with a remainder instead | The odd-and-even test fails: a remainder keeps the sign of a negative angle, so every angle before nought reflects rather than wrapping |
| The series is long enough — **control passed, and the reason is recorded** | Cut it from nine terms to five | *Nothing failed.* The result narrows to thirty-two fractional bits and five terms is already inside that. Nine is set for the wide value, not the narrow one, and the comment now says so |
| The K-weighting runs | Pass the signal through untouched | Three tests fail, both compliance cases among them |
| The high-pass runs — **control passed, gap was real** | Drop the second filter stage | *Nothing failed.* Every test used a 1 kHz tone, where that stage costs 0.03 dB. A 50 Hz test now exists, where it costs 3.9, and the same mutation fails it |
| The standard's offset is applied | Drop the -0.691 | Three tests fail; every reading sits 0.7 units high |
| Channels are summed, not averaged | Divide by the channel count | Four tests fail, including the one that pins a stereo pair at 3.01 units above mono |
| A block is four steps of overlap | Make it three | Five tests fail, both compliance cases among them: a block divided by a block's length must hold a block's energy |
| A peak survives every zoom | Average the extremes going up, as a naive downsample would | Two tests fail: the single click in half a second of silence is gone by the second level, and the widest zoom no longer agrees with the samples |
| A block holds two sides, not a magnitude | Store the reach and mirror it | Four tests fail, including the one that asserts a block reports samples that are actually in it |
| The mean square does not compound with zoom | Fold stored means instead of exact sums | The within-one bound fails at the levels above zero, where a floor of floors has drifted |
| The last block is divided by what it holds | Divide it by a full block's width | Two tests fail: three samples at a steady level read quieter than a full block of the same |
| Levels are folded within a channel | Pair across the channel boundary | Two tests fail — but only after the fixture was given an odd block count. See below |
| The pyramid reaches a single block | Stop one level early | Five tests fail, the file-shape check among them |
| Dependencies run downward | Add one from `media` up to `io` | `make lint` names the crate, the layer it is in, and the layer it reached for |
| Dependencies do not run sideways | Add one from `media` to `model`, its own layer | The same refusal: sideways is a violation, because two crates in a layer depending on each other are one crate that has not admitted it |
| The declared layers are the real ones | Restore the order the diagram used to draw | Three findings, which is the proof the old diagram was wrong rather than differently drawn |
| Every crate is declared | Drop `io` from the layer block | Two findings: the crate is in the tree and not the layers, and something depends on a crate that is not declared |
| A summary file is checked | Drop the digest comparison | The byte sweep and the shape test fail |
| A summary's digest covers its header | Hash the blocks only | Four tests fail, and a probe named the cost exactly: byte 8 and bytes 32 to 63 become undetectably editable — the sample rate, and the digest of the sound the summary is *of* |
| A summary's length follows from its header | Accept bytes past the shape | The trailing-bytes test fails: a summary's length is derived, so extra blocks are a disagreement rather than padding |
| Reserved fields are refused when set | Read them and ignore them | Twelve reserved bytes are accepted, one test names each |
| The block size obeys the summary's own rule | Check it against nought only | The header test fails on a block size of a hundred, which is not a power of two |
| A curve holds past its ends | Continue the slope before the first keyframe | The holding test fails: a parameter set to reach 100% arrives at more than that outside the keys that describe it |
| A curve inverts its horizontal Bézier | Use the time fraction as the parameter directly | The bent-handle case fails: eleven sixteenths along a span is where `t = 1/2` lives, and skipping the inversion reads it as eleven sixteenths |
| The inversion includes an exact hit | Make the bisection comparison strict | Three tests fail; every eased value lands one dyadic short |
| The segment search is not off by one | `<` for `<=` on the midpoint | Two tests fail, including the one that asks a curve for its own keyframes |
| Ease handles stay inside their span | Accept any horizontal | The refusal test fails; a folded curve has more than one value at an instant |
| The ease rounds to nearest — **control passed, gap was real** | Round towards nought | *Nothing failed.* The rule was stated in the code and pinned by no test. A case where the exact value is 1/10, which is 104857.6 parts in 2^20, now exists and the same mutation fails it |
| The interpolation form matters — **control passed, and the reason is recorded** | Write `from(1-f) + to·f` instead of `from + (to-from)f` | *Nothing failed*, correctly: with exact rationals the two forms are identical. The comment says as much and says why the habit is kept anyway — in fixed point they are not |
| Automation multiplies a dissolve | Drop the track's opacity at a transition | The dissolve-under-automation test fails: two things decide a layer's opacity there and either alone throws the other away |
| An opacity saturates rather than exceeding | Leave the curve's overshoot unclamped | The overshoot test fails; a track reads as more than fully opaque |
| An opacity curve reaches the file | Write the absence of one instead | Two tests fail — and, importantly, *not* the round-trip tests. See below |
| An unknown interpolation is refused | Read it as a hold | The unknown-tag test fails: an ease would become a step while the file still said ease |
| A sound track has a fader, not an opacity | Accept one on either kind | The refusal test fails |
| A fader ramp's interval is half open | Close it, arriving on the last sample | Three tests fail: the last sample lands on the target it should stop one step short of |
| A fader ramp actually moves | Hold the block at its starting value | Four tests fail, the tiling test among them |
| A fader ramp runs the right way | Swap its ends | Six tests fail; a fade down goes up |
| A moving fader still reports clipping | Count clipped samples as nought | Three tests fail, including the one that mixes a rising ramp against a full-scale source |
| The mixdown ramps within a frame | Hold each frame flat at its starting gain | Three tests fail: a fade over eight thousand samples takes four distinct values |
| Mute wins over automation | Let the curve speak for a muted track | The mute test fails; a track turned off comes back on because somebody drew a fade on it |
| An automated fader saturates | Leave the curve's overshoot unclamped | The saturation test fails — and only after a fixture existed that could overshoot. See below |
| The level curve reaches the file | Write the absence of one instead | The lane test fails — and only after the fixture animated *both* lanes. See below |
| The two lanes are written in order | Swap them | Six tests fail, the canonical-encoding test among them |
| Removing the last keyframe turns the lane off | Leave the lane on with an empty-looking curve | Two tests fail: undoing the first keyframe leaves a flat curve nobody asked for |
| A moved keyframe lands in its new place | Re-insert it at the index it came from | The reordering test fails; the curve is no longer in time order |
| A keyframe cannot land on another | Drop the collision check | The collision test fails: two values at one instant is the same nothing as none |
| A lane belongs to one kind of track | Accept either lane on either kind | The lane test fails |
| Setting a keyframe hands back the old value | Hand back the new one | The journal's own check fails it as `HistoryInconsistent`, which is what that check is for |
| Tetrahedral is not trilinear | Make it call the other one | Two tests fail; twenty-nine of thirty-nine greys pick up a tint |
| Each tetrahedron has the right vertices | Give one branch the wrong ones | Two tests fail, the continuity one among them — but only after that test existed. See below |
| A sample picks the right tetrahedron | Swap two of the six orderings | Two tests fail |
| A colour outside the table is held at its edge | Drop the clamp | The saturation test fails |
| The top of the range reaches the far corner | Read it as the start of the last cell | Four tests fail |
| The cube is indexed red-fastest | Transpose it | The identity test fails; a table that changes nothing changes everything |
| A `.cube` file is written red-fastest | Transpose the writer | Two round-trip tests fail against a fixture asymmetric in all three axes |
| A `.cube` domain is checked | Read the lines and drop the refusal | The domain test fails: a table authored for another input range would be the wrong look on every pixel |
| A one-dimensional table is refused | Skip the key like any other | The refusal test fails; a curve's samples would be read as a cube's |
| An over-range sample is not clamped | Clamp it into nought to one | Two tests fail; a highlight sent above white on purpose is flattened |
| A decimal is read to nine places | Keep three | Four tests fail, including the round trip, because the digits no longer survive |
| Samples come after the size | Accept them before it | The no-size test fails |
| A look is applied in the encoding it was made for | Feed any frame to the table | The encoding test fails; a show LUT would run on the wrong space with nothing crashing |
| A look needs straight coverage | Accept premultiplied | The coverage test fails: a non-linear function on premultiplied samples computes `f(ac)` where `a·f(c)` was wanted |
| Coverage is carried, not written | Write 255 into the alpha byte | The keyed test fails — the same mistake that made every keyed frame a solid rectangle in the conversion path |
| A look needs three colour channels | Accept any format | The format test fails |
| Colour channels are normalised as colour | Normalise them as chroma | Two tests fail, including the one where an identity table must change nothing |
| The footprint tool sees the arena | Halve `HEAP_BYTES` | It reports 34 pages rather than 42, with `.text` unmoved — which is also the measurement of what the arena costs |
| The footprint counts what is loaded | Leave `.bss` out of the loaded sections | It reports 25 pages, under-counting by the whole arena |
| A short lookup table file is refused | Pad it out to a full cube | Three tests fail, both sweeps among them |
| A fourth number on a sample line is refused | Ignore it | The sample-line test fails |
| A lookup table's lines are bounded | Remove the bound | Two tests fail |
| The documents' counts match the tree | Put a crate's count one behind | `make lint` names the crate, what it says, and what the tree holds |
| Every crate with tests states a count | Delete one row's count | It names the crate and says its row states none |
| The README's total matches | Put it one behind | It names both numbers |
| The README's control count matches | Claim two hundred | It names both numbers |
| A new test cannot land undocumented | Add one and change nothing else | Two findings, one per document, which is the case this exists for |
| A split gives both halves the look | Rebuild the tail from scratch | Two tests fail, including the one that says join is the inverse of split |
| Differently graded clips do not join | Stop comparing the grades | The join test fails; one look is discarded without saying so |
| A grade reaches the file | Write "no grade" for every clip | The grade round-trip fails |
| An unknown grade flag is refused | Read it as no grade | The flag test fails; a look is dropped while the file still says there is one |
| A grade reaches the layer stack | Report `None` on every layer | The stack test fails; a look nothing can apply |
| A look node names which look | Leave the digest out of its identity | The edited-grade test fails; the second render is answered out of the pool with the first |
| A look's identity is more than its samples | Leave out the interpolation, then the encoding | The identity test fails either way: a table read two ways is two looks |
| The timeline puts a grade in the graph | Ignore the clip's grade | Two tests fail — but only after they existed. See below |
| A graded layer is fetched straight | Fetch it premultiplied | Two tests fail; a look refuses the frame the compositor wants |
| A graded layer is re-associated | Leave it straight | Two tests fail; the render does not end premultiplied |
| The slate's picture depends on when it is rendered | Move the playhead one frame | The golden fails — but only after a fade existed. See below |
| The slate's picture depends on the fade | Never apply the track's opacity | The golden fails |
| Higher tracks are on top, on the target too | Swap the two layers | The golden fails |
| A capture's checksum covers the chunk name | Cover the data only | The CRC test fails against a checksum computed from the polynomial |
| A stored block carries its complement | Write the length twice | The stream test fails on the one redundancy a stored block has |
| Adler-32 uses the right modulus | Take it modulo 65,536 | The wrapping test fails — but only after a fixture existed whose sums reach a modulus at all |
| Every scanline is its own | Write the first row for all of them | The same test fails — but only after a fixture existed whose rows differ |
| A capture's rows are unfiltered | Claim a filter the rows do not use | Two tests fail |
| A capture refuses premultiplied coverage | Write it as though it were straight | The coverage test fails |
| The record timecode is the order, not the event number | Sort by the number | The renumbered-and-reversed test fails — but only after a fixture existed whose numbers disagree with the record |
| A label is recounted at the rate the caller stated | Take the parser's frame number as it stands | The round trip fails, by a quarter, for every list at 24 |
| A dissolve opens half its length, rounded down | Round it up | The odd-dissolve test fails: the picture moves by a frame |
| A dissolve gives the outgoing clip its tail back | Leave the clip as the event wrote it | The outgoing clip comes back twelve frames short |
| A dissolve's incoming clip starts early into its handles | Write its in point as the cut | The event's source in point is twelve frames late |
| A reel and its comment are two statements about one source | Do not compare them | The disagreeing-reel test stops refusing |
| Two sources may not share a reel name | Accept the second | The collision test stops refusing |
| Picture is written before sound | Never notice the order | The out-of-order test stops refusing |
| A trailing gap is reported as left behind | Do not count it | The trailing-gap test reports nothing lost, and the cut still comes back short |
| An out point is exclusive | Write one frame more | The source and record spans are each a frame too long |
| Coverage is an area, not a sample at the centre | Test the pixel's centre instead | The partial-column test fails: a quarter becomes nothing |
| The shoelace area is halved | Leave the sum doubled | Every whole pixel reads two |
| The closed form subtracts what runs past both sides | Subtract only the first | The two implementations stop agreeing |
| The closed form measures from the corner the normal points from | Always measure from the top left | The same, at every orientation with a negative coefficient |
| Quantising rounds half away from zero | Truncate | Half of 255 reads 127 rather than 128 |
| A coverage plane runs across before it runs down | Fill it column by column | The reading-order test fails |
| A mask scales the coverage with the colour | Scale the three colour channels only | The masked frame claims more colour than its coverage allows |
| A coverage plane is one byte per pixel | Do not check its length | A plane of the wrong size stops being refused |
| A wipe leaves both of its clips whole | Fade the incoming one as a dissolve would | The incoming clip shows through the outgoing one on the covered side |
| The wipe travels with the incoming layer | Report no wipe at all | Every wipe renders as a dissolve |
| A track's automation multiplies a wipe too | Apply the transition alone | A wipe inside a fade stops being faded |
| The file says which kind of transition | Write every kind as a dissolve | A wipe comes back a dissolve |
| An unknown transition tag is refused | Read anything unrecognised as a dissolve | A resealed file with a tag of two stops being refused |
| The renderer applies the wipe | Ignore the layer's wipe | A wipe and a dissolve become the same picture |
| A wipe's edge starts where its direction points from | Sweep from the far corner instead | The wipe covers everything at nought and nothing at one |
| A wipe is reported as left behind | Do not count it | A list that lost the edge reports nothing lost |
| The band is weighted by where in it each part lies | Weight the whole slab at a half | A pixel inside the band stops being the ramp at its centre |
| The first moments are area times centroid | Subtract the two coordinates instead of adding them | The same test fails |
| The band is centred on its edge | Start it at the edge instead of half a band before | A soft edge and its complement stop summing to one |
| The file carries a wipe's softness | Write nought for every wipe | A third comes back as a hard edge |
| A softness outside its range is refused | Do not check it | A softness of three halves stops being refused |
| The renderer is told the softness | Pass nought | Every soft wipe renders hard |
| A concave outline is refused | Follow whichever way the last corner turned | An arrowhead stops being refused |
| Corners in a line enclose nothing | Accept a polygon that never turns | Three collinear points become a mask |
| A mask survives a rebuild | Drop it in `with_source` | A slip loses the shape |
| A mask's winding is measured, not assumed | Take one direction as given | The same shape given the other way round inverts |
| An inversion flips the byte, not the shape | Ignore the flag | A mask and its inversion stop summing to full coverage |
| A mask's corners are fractions of the frame | Read them as pixels | A half-by-half rectangle stops covering a quarter |
| The file carries a clip's mask | Write the absent flag for every clip | A shape does not come back |
| A file's mask goes through the model's constructor | Build a rectangle instead | A file describing an arrowhead stops being refused |
| The renderer applies the mask | Ignore the layer's shape | Nothing is taken away |
| One asset per digest | Insert every asset unconditionally | The same content added twice becomes two identifiers |
| One digest cannot be two lengths | Accept whichever arrived first | A contradiction stops being refused |
| A hint is not part of what makes an asset the same | Overwrite the record's hint on a second add | Opening a file rewrites a project nobody edited |
| Moving a hint gives back the one it replaced | Return nothing | The caller cannot put it back |
| A hint that says nothing is refused | Accept no bytes at all | An empty hint looks like an answer |
| The file carries a location hint | Write the length and drop the bytes | A hint does not come back |
| A file listing one asset twice is refused | Let the reader fold them together | Clips indexing the second record point at the first |
| A planner asks whether media is there before naming it | Always name the source | An unavailable source is fetched, and the slate lands in the cache under the picture's key |
| An offline slate's period is a fraction of the frame | Fix it at sixteen pixels | The slate is a solid colour on a small frame |
| An offline slate runs diagonally | Vary along one axis only | It becomes bars, which a programme may contain |
| Each test pattern has its own identity | Give two the same tag | Five patterns produce four identities |

### A rule stated in a comment is not a rule

The curve's ease rounds half away from zero, and the code says so, and the
reason is good: the compositor rounds the same way, so a fade drawn by a curve
and a fade drawn by the compositor agree at the point where they meet.

Rounding towards nought instead failed nothing. Every test happened to use a
value that was already a multiple of the precision, so the two rules gave the
same answer everywhere the suite looked, and the rule that mattered was held up
by a sentence.

The fix was to find a case where the rules disagree and pin it: a tenth is
104857.6 parts in 2^20, so nearest is 104858 and towards nought is 104857. That
took deriving one value by hand, which is the price.

The general shape: **a rule that only a comment states is a rule that will
change without anybody noticing.** Rounding direction, tie-breaking, saturation
versus wrapping, the end a half-open range excludes — each is easy to write
down, easy to be right about, and easy to have no test for, because the obvious
test cases are the ones where it makes no difference.

### The field that detects staleness can itself go stale

The summary file's digest covers its header as well as its blocks, and it would
have been easy to hash only the blocks — they are the data, after all. A probe
against that version named the cost precisely rather than plausibly:
thirty-three header bytes become undetectably editable. Byte 8 is the sample
rate, so a 48 kHz summary reads as 44.1 and every block silently covers a
different span of time. Bytes 32 to 63 are the digest of the sound the summary
is *of* — the field whose whole purpose is to let a reader see that the summary
is stale.

A staleness check that can itself go stale is not a check. The general form:
**a field that exists to detect corruption has to be inside whatever detects
corruption**, and "it is only metadata" is the argument that puts it outside.

The number came from running the mutation and listing which bytes were accepted,
not from reasoning about which ought to be. Reasoning would have found the rate
and quite possibly stopped there.

### A drawing is not a check

The architecture document drew `sapstudio-io` beneath `sapstudio-media` while
the manifest had it depending on `sapstudio-media`, and had done for as long as
there was an `io` crate. Nobody was misled, because nobody consults a diagram
to find out what compiles — which is the point. The diagram was not wrong in a
way that hurt; it was wrong in a way that *could not be found*, because nothing
read it.

Every other rule in this project is enforced by something that runs. The
layering was the exception, and the exception is where the drift was.

So the layers are now declared in a fenced block that `tools/layering.py`
parses, next to the diagram that renders them for a reader. The check refuses
any dependency that does not run strictly downward, and it refuses a crate that
is in the tree but not the block. Restoring the old order produces three
findings, which is the evidence that the old drawing was wrong and not merely
drawn differently.

The general rule this is an instance of: **a document that states a fact about
the code should be readable by the code.** Prose for the reasoning, a
machine-readable block for the fact, and one check that they agree.

That check found a bug in itself on its first run, which is worth recording
too. It read `[[bin]] name = "sapstudio"` as the package name of the image
crate and reported a crate missing from the layers. It was right to: the
finding was real, the cause was the reader rather than the tree, and a check
that had happened to skip that manifest would have passed and told nobody.

### A fixture can be too tidy to break

The control that pairs summary blocks across a channel boundary failed nothing
at first. The mutation was real and the test was aimed at exactly it — but the
fixture was eight blocks long, so every level had an even count, every block had
a partner within its own channel, and the boundary was never reached. Eleven
blocks halve to six, three, two, one: two odd counts, two blocks with no
partner, and the same mutation fails immediately.

Powers of two are the natural length to reach for and are the one length that
cannot exercise a halving's remainder. The same applies to a frame count that
divides the sample rate, a buffer that is a whole number of blocks, and a
sequence whose clips all start on the second. **A fixture whose dimensions
divide evenly tests the easy half of every function that divides.**

That the mean-square test caught it anyway is luck, not coverage: its fixture
happened to be thirty-four blocks long. A test that catches a bug it was not
aimed at is a reason to go back and fix the test that was.

### A round trip proves less than it looks like it proves

Skipping the gamut matrix was not caught by the conversion's round-trip tests,
because a matrix skipped in both directions is also a round trip. It was caught
only after a test was added that asserts what a *single* conversion must do —
BT.709's red sits well inside BT.2020, so expressing it there needs green and
blue as well, and a pipeline that skipped the matrix hands back pure red.

The general lesson, which applies to every symmetric operation in this project:
a round trip checks that two functions are inverses, not that either is
correct. Both need a test that looks at one direction on its own.

A second instance, found later and worth adding here rather than starting a new
heading. Dropping the opacity curve from the project file entirely — writing
"no automation" for every track — failed neither of the round-trip tests:
neither `a_project_survives_a_round_trip` nor `the_encoding_is_canonical`.
Both compare a round trip against *another
round trip*, so a field the writer never writes is missing from both sides and
they agree perfectly. It was caught only by a test that names the field, reads
it back, and checks the values it holds.

So: a round-trip test cannot see a field the format has forgotten. Every
field a format carries needs a test that names it.

**And then it happened again, one commit later.** A sound track's level curve
was added to the format, and dropping it from the writer failed nothing — for
exactly the reason written above, which had been written above at the time. The
fixture animated the picture track and not the sound one, so the lane the
mutation removed was a lane no test ever put anything in.

Writing a lesson down is not the same as applying it. The operational form,
which is narrower and harder to skip: **when a format gains a field, the
fixture gains a value for it in the same commit** — otherwise the sweeps cover
bytes that are never written, and every test agrees about a field that is not
there.

### A fixture that does not vary along the axis under test

Two controls on the capture writer failed nothing, and both had the same cause
in a new shape.

Writing every scanline as the first one broke nothing, because the fixture was
a horizontal ramp — whose rows are all identical. The comment above it even
said "a test pattern differs everywhere", which is true across and false down.

Taking Adler-32 modulo 65,536 rather than 65,521 broke nothing either, because
the fixture was thirty-nine bytes and neither accumulator ever reached a
modulus. The test computed the checksum independently, from its definition, and
still could not tell the two apart — an independent computation over data that
does not exercise the difference is not an independent check.

The general form, and the third time this project has met it: **a fixture that
does not vary along the axis under test cannot see a change to that axis.** It
was powers of two hiding a halving's remainder, then a static composite hiding
the instant, and now identical rows hiding a row index and small sums hiding a
modulus. The question to ask of every fixture is not "is this valid input" but
"does this input move when the thing I am testing moves".

It has since been met a fourth time, in the fourth shape: the conform suite's
test that an importer reads the record timecode rather than the event number
handed the events over backwards and passed with the sort mutated to use the
numbers — because reversing a list without renumbering it leaves the numbers
ascending with the record, so the two orders agree and either sort gives the
same answer. The fixture varied along the *order* axis and not along the axis
that separates the two readings of it. The events are now renumbered to agree
with the wrong order, so the numbers are a complete, self-consistent account of
a cut that runs the other way, and the control fails.

### A fallback inside a cached computation poisons the cache

A source node's identity covers the media, the tick and the description — not
whether the file was reachable. So a node that fell back to an offline slate
while evaluating would store that slate under the real picture's key, and serve
it after the drive came home.

This is worth stating as a shape rather than as one bug, because it applies to
every cached pure function: **anything a computation reacts to must be in its
key, or must be decided before the computation is named.** The second is nearly
always the better answer — availability changes, and putting it in the key
would make every cache entry useless the moment a drive was unplugged.

The test that holds it renders twice through *one* pool, absent then present,
and requires the second render to be the picture. A test that used a fresh pool
each time would pass with the bug in place.

### A slate has to read at the size it is drawn

The offline pattern's stripes were sixteen pixels apart, which is a solid
colour on a frame narrower than sixteen pixels — and that is exactly the size
the freestanding image renders at, and exactly the case where a slate being
mistaken for a shot of a red wall matters. A four-pixel test found it.

The period is now a fraction of the frame. The general form: **a pattern whose
job is to be recognised has to be defined relative to the picture, not in
pixels**, because the picture's size is not something it gets to assume.

### A theorem can be true of everything it was tested on and false

Conform's claim — *if the export leaves nothing behind, the round trip is
equal* — was stated three milestones before the case that breaks it was tried.
Two identifiers naming one digest: the export reports nothing lost, the import
resolves both clips to whichever identifier it finds first, and the sequence
comes back pointing at one of them.

Every test of that theorem passed, and none of them built a project holding the
same content twice, because nothing in the fixtures ever did that and nothing
in the model prevented it. The theorem was not wrong about anything it tested.
It was wrong about the world it assumed.

What found it was reading a **document** — a block in `ARCHITECTURE.md` that
described a media asset as carrying a location hint the type did not have.
Checking that claim led to looking at what the type *did* have, which led to
asking what makes two assets the same, which is the question the theorem
depends on and had never been asked.

The operational form: **a theorem's assumptions are worth writing down
separately from the theorem**, because the tests will only ever exercise the
world the fixtures build, and the assumption is exactly the thing no fixture
thinks to violate.

And the second form: prose is worth *checking* like a claim. This project has
a tool that refuses when the documented test counts disagree with the tree, and
nothing at all that notices when a diagram describes a type that does not
exist. Reading one carefully found a bug that four hundred tests did not.

### A picture, kept beside its hash

This document has said since its first version that a reference frame is stored
beside its hash *so that a failure can be looked at, not just counted*. For a
long time it could only be counted, and the reason was honest — the only frame
the freestanding image composites is sixteen pixels wide.

`tests/golden/reference.png` is the first one that is a picture: 320×180,
colour bars underneath, a ramp and a flat colour meeting at a soft wipe, both
inside a six-sided mask. On a mismatch the test writes what it actually
rendered beside the reference and names both paths. That is the difference a
picture makes over a digest: a hash says something changed, two files say what.

And its first version showed **nothing of what it claimed to show**. Both sides
of the wipe rendered the same test pattern, so the feathered edge the capture
existed to demonstrate was perfectly invisible — the same fixture lesson, for
the fifth time, in the place most likely to go unnoticed because the image
still *looked* fine. The capture now asserts that the two sides of the wipe
differ and that a band of partial values lies between them, so a reference that
stops demonstrating its subject fails rather than being quietly admired.

### Measured against the previous commit, not against reasoning

The slate's golden moved again when a clip gained a mask flag, and the obvious
explanation — one byte per clip, three clips, three bytes — was checked in a
**git worktree at the previous commit**: one clip grew by one byte, three by
three, seven by seven.

The check mattered more than the last two times, because this commit changed
both the version *and* the payload, and an earlier attempt to isolate them by
reverting only the version constant proved nothing — the mask bytes were still
being written, so both builds produced the same file. A "before" has to be a
real before.

### A control that passes can be worth more than the control was

The soft edge's zero case delegates to the hard path rather than dividing by a
band of nothing. The control for it — delegate to a band a thousandth of the
travel wide instead — **changed nothing**, and the reason is the interesting
part: at that width every pixel is already fully in or fully out, so the two
planes agree byte for byte.

Which means the delegation is a *convenience* rather than a patch over a
discontinuity, and the soft path **converges** on the hard one rather than
jumping to it. That is a stronger and more useful statement than the control
was trying to make, so it became a test — with a second half asserting that a
band wide enough to see *does* differ, so the comparison is not measuring a
function that ignores its argument.

This is the second time a passing control has been the finding rather than a
failure of the fixture (the first is two sections below, on claims that turned
out to be decoration). The two cases are different and both are worth knowing:
sometimes a control that changes nothing means the claim above it is empty, and
sometimes it means the claim is true for a better reason than the one written
down.

### A design rejected for a reason that was wrong

The shape rasteriser left soft edges out and recorded why: "the area weighted
by a ramp rather than a plain area", which is "a much larger case analysis".
That was written with conviction and was simply false.

The integral of an affine function over a polygon is its area times its value
at the polygon's centroid — the definition of a centroid, not a result about
one. The ramp is affine, the region is the pixel square clipped by two parallel
half-planes, and the clipper already existed. It is two clips and a moment.

Worth recording because a **reason** attached to a deferral is a claim like any
other, and nothing in this project checks the reasons. A test can fail; a
paragraph explaining why something was not built cannot. The operational form:
when a deferral's reason is technical rather than about priorities, it is worth
five minutes of actually trying, because the cost of being wrong is a feature
that never gets built and a document that confidently says why.

### A refusal behind a checksum needs a resealed fixture

`SPRJ` refuses an unknown transition tag, and the first test for it mutated the
tag byte in a real file and asserted the refusal by name. It could never have
seen it: the format's digest covers the payload, so a mutated byte is refused
as a **digest mismatch** before a single field is parsed — which is exactly
what the byte sweep two sections above asserts, on purpose.

A test for a field-level refusal in a digested format has to **reseal** the
file: change the byte, recompute the payload digest, write it back into the
header. That is not a contrivance — it is the case that matters, because a file
whose digest agrees with its contents is one something produced deliberately,
and that is precisely when the field checks have to hold.

The general form: **a check that sits behind an integrity check cannot be
tested by corruption.** Every format here that carries a digest has this
property, and every future test of one of its interior refusals needs the same
treatment.

### A golden that moves for a reason worth checking

Bumping `SPRJ` to version seven moved the slate's golden transcript. The
obvious explanation — the version byte is in the header and the slate's digest
covers the whole file — is also the correct one, but it was *checked* rather
than assumed: encoding one transition-free project under both versions produces
files of the same length that differ in exactly one byte, at offset four.

Worth the two minutes because the alternative explanation was live. The same
commit changed how transitions are written, and a golden that moved because the
payload changed in some way nobody had characterised would look identical from
here. Updating a golden is the one moment where a test stops being able to tell
you anything, so the reason has to be established before the number is
replaced, not after.

### A test over black cannot tell light from code values

The wipe's first test put white over black and asserted that the edge pixel
lands above the code-value midpoint, because mixing in linear light is
brighter. It read exactly 128 — the midpoint — and the assertion was wrong
twice over.

Over black the bottom layer contributes **no light at all**, so the result is
just the masked top layer encoded again, and the linear-light answer and the
code-value answer are the same number. This project's notes already carried
that finding once, about a compositing test; making it again in a new module
is what an already-recorded lesson costs when it is recorded as a fact about
one test rather than as a rule.

The rule: **a test whose background is black cannot distinguish compositing in
light from compositing in code values.** Put something with light in it
underneath.

And the direction was wrong too. Over a mid-grey the linear answer is 154 and
the code-value answer is 192 — linear is the *darker* one here, the opposite of
what the white-over-black intuition predicts, because stored 128 is only 0.216
of full light. The test now asserts the number, derived by hand, rather than an
inequality derived from an intuition.

### Two claims that were not load-bearing, established by trying

Both are in the rasteriser and both were written as though they mattered.

That the boundary line **belongs** to the region: making the test strict
changes no coverage anywhere, because a line has no area and the clipper puts
back as a crossing point exactly the corner the strict test would have dropped.

That the early exit when a polygon is clipped below three vertices **guards**
anything: the shoelace sum over two vertices is already nought, and clipping an
empty polygon yields an empty one, so removing it changes no answer. It is a
real optimisation — a sixty-four edge mask would otherwise keep clipping
nothing for every pixel outside it — and it is now labelled as one.

Neither is a bug. Both were comments claiming more than the code delivers, and
the only reason either was found is that a control was written for it and the
control passed. **A control that changes nothing is not always a bad fixture;
sometimes it is a true report that the claim above it is decoration.** The
outcome is the same either way: something has to change.

### A round trip that does not go through the file is not one

`conform::import` read the frame numbers off the timecodes as they arrived and
was wrong by a quarter for every list at 24, because `edl::parse` labels every
timecode at thirty — it has no rate to use and will not invent one, which
M3.5 states plainly. The label is four numbers; what they count in is told.

Every test that handed the exported list straight back to the importer passed.
They were not round trips. A value compared with itself agrees about everything,
including the parts of it that were never written down, and the rate is exactly
such a part: it lives in the `Timecode` and not in the file. Only the tests
that went **export, write, parse, import** could see it, and they failed
immediately.

So: a round trip is verified through the serialised form, never through the
in-memory value the writer produced. The same rule the project file already
follows for a different reason — a `SPRJ` test that skipped the bytes would
never test the bytes — restated here for the reason that bit: the bytes are
where the information is *lost*, and testing around them tests nothing.

### A golden hash of something that does not change

The slate began rendering a picture and reporting its digest, which is the
golden render hash `## Golden output` asks every render to carry and which
nothing here had. The first version pinned a project whose two clips both ran
the whole span — so every instant composited identically, and a control that
moved the playhead a frame broke nothing.

A golden hash of a static result pins the arithmetic and says nothing about the
inputs it claims to name. The fix was to put a *fade* on the upper track, which
makes the picture a function of the instant and, incidentally, puts the curve
arithmetic in the image: exact rationals, an interpolation, and a fixed-point
opacity reaching the compositor, on the target rather than only on the host.

The general form, which applies to every golden in this project: **a golden
output is only evidence for the inputs it actually varies with.** Naming an
input in the comment does not make the hash depend on it.

### The same lesson, one commit later

The heading below — that a number recorded repeatedly acquires a story nothing
checks — was written, and then the same mistake was made immediately.

`Node::Look` was deliberately *not* built in one commit, on the reasoning that
it would cost the image about two pages: `evaluate` would reach the lookup
table code, and the freestanding image links `evaluate`. That reasoning was
stated confidently and was wrong in both halves. The image links neither
`evaluate` nor any other symbol from `sapstudio-render` — the slate exercises
the model, the reel, the pool and the test patterns, and never renders. When
the node did land, the footprint did not move by a byte.

The same check that found it also falsified the platform contract's
explanation of an earlier two-page rise, which said the same thing about the
same crate.

Writing a lesson down does not install it. What installs it is doing the thing
the lesson says at the moment it applies — and the moment it applied here was
a sentence beginning "it would cost", which is the shape a guess takes when it
is about to be recorded as a fact. **An estimate about a measurable quantity,
in a project that measures it on every build, is a decision not to run the
tool.**

### A wiring nothing tested, and a fixture that hid why

The graph gained a `Look` node, the model gained a grade, and `timeline::plan`
was taught to put one in front of the other. Deleting that wiring entirely
broke **no test**. The node had tests, the model had tests, and the join
between them had none — which is the seam that always lacks them, because both
sides look covered.

Writing the missing test found the real fault underneath. A look refuses
premultiplied coverage, and the timeline renders premultiplied, so a graded
clip could not render *at all*. The two want opposite things for good reasons:
`over` is only correct on premultiplied samples, and a non-linear function on
premultiplied samples computes `f(ac)` where `a·f(c)` was wanted.

The fix is to fetch a graded layer **straight**, grade it, and associate it
afterwards — which loses nothing, because the frame was never premultiplied.
Unpremultiplying one that had been would; that is why `Look::apply` refuses
rather than doing it quietly.

And the fixture had been hiding the collision. `Flat::frame` ignored the
description it was asked for and always answered with its own — harmless while
every layer was fetched identically, and exactly wrong the moment one was not.
One test depended on that fault deliberately, to prove the graph refuses a
source that answers a different question; every other test depended on it by
accident. The lie is now a field on the fixture, set only where it is the
subject.

**A fixture that ignores one of its inputs is a fixture that cannot see a
change to that input.** It reads as simplification and behaves as a blind spot.

### `new` is the only place allowed to start from nothing

Adding one field to `Clip` broke it in three places, and they were three
spellings of the same thing: `Item::with_duration` rebuilt a clip field by
field, `Item::split` built its tail with `Clip::new`, and `Edit`'s slip built a
whole new clip from three of the old one's fields. Every one of them silently
dropped the new field.

Two were found by reading and the third by a test, which is two more than
should have needed finding. The patch-each-site fix would have left the trap
armed for the next field.

What removes it: **a constructor starts from nothing, and everything else
changes one field.** `with_grade`, `with_source` and `with_duration` each take
`..*self`, so a field added tomorrow travels through all of them without
anybody remembering. `Clip::new` is the one place that begins empty, which is
correct there and nowhere else.

The general form, for any value type that gains fields over time: count the
places that *rebuild* it. If there is more than one, the next field will be
dropped by all but the one that gets remembered.

### A number kept by hand is a number that goes stale

Every crate's test count in the architecture table, and the total and the
control count in the README, were maintained by hand for the first five hundred
tests. None of them was ever wrong, and that is luck rather than process:
nothing checked them, and the only reason they stayed right is that updating
them was on a mental list.

`tools/counts.py` now reads them and refuses a disagreement — the same bargain
as `layering.py`, and for the same reason. It made one sentence in the README
change from words to digits, which is a small loss of prose for a fact the code
can read, and that trade is the whole idea: **prose for the reasoning, a
machine-readable fact beside it, one check that they agree.**

The count is static — `#[test]` attributes rather than a run — and that is
checked rather than assumed: at the commit that added the tool, the static and
runtime counts matched exactly for all nine crates. `make verify` runs the
suite anyway, so a divergence would show up there as a different total.

### A sweep that had to mutate inside the field's alphabet

The `.cube` parser had no sweep at all — the three binary formats each have
two, the edit decision list has one, and this project's own rules say a parser
without a target does not ship. That gap was found by going and looking, not by
anything failing.

Writing the sweep then produced three failures, and all three were the
*assertion* being wrong rather than the parser.

The sharpest one: a text sweep that replaces the `0` of `0.0` with a space
gives ` .0`, which splits to `.0`, which is still nought. Different text, same
number — and the sweep read that as "this byte carries nothing", which is what
it is meant to report for a byte the reader dropped. The mutation has to stay
inside the alphabet of the field it is mutating. A digit changed to a
*different digit* always changes the number it spells, and the claim is sharp
again.

The general form: **a text sweep that mutates outside a field's alphabet
measures the lexer's leniency rather than the parser's completeness.** Binary
formats do not have this problem, which is why the technique transplanted
badly.

### A format that cannot detect its own truncation

The same sweep found something about `.cube` rather than about the reader. A
prefix that stops before the last sample line is refused — the cube comes up
short and the count says so. A prefix that stops *inside* the last line can
still spell three numbers: `200.0 0.0 200.0` cut to `200.0 0.0 2` is three
numbers, and cut to `200.0 0.0 200.` is three numbers with the same values.

No reader could do better. `.cube` carries no length and no digest, so a file
truncated inside its final number is indistinguishable from a valid file
somebody authored differently.

Every format this project writes itself carries both, and this is exactly why:
`SPRJ`, `SPRW` and `SPPK` refuse *every* prefix, and this one cannot. That is
the argument for a length and a digest stated as a measurement rather than as a
principle — an interchange format without them has a class of corruption that
is undetectable by construction, and the test now says where the line falls
instead of asserting something convenient.

### A number that was being read as the wrong thing

The image's footprint has been recorded after every change — thirty-six, then
thirty-eight, forty, thirty-nine, forty-two — and read each time as "the
program has grown". Taking the number apart showed that reading was wrong in a
way that mattered: **sixteen of the forty-two pages are one constant**, the
static arena in `sapstudio-rt`, which is a reservation rather than anything the
program contains. The code went from twenty pages to twenty-six over the same
period, and sixteen pages never moved.

Worse, the conclusion drawn from the number — that the answer is to split the
program so the image links fewer crates — was aimed at the smaller half. The
arena alone is eighty-four per cent of what a Sapote program is given.

Nothing here was a *bug*: every measurement was correct and every entry in the
table was true. What was wrong was the sentence wrapped around them, and no
test can catch a wrong sentence about a right number. What catches it is
breaking the number down and looking, which `make audit` now does on every run.

The general form: **a single number that gets recorded repeatedly acquires a
story, and the story is not checked by anything that checks the number.** When
a figure is worth tracking it is worth decomposing at least once, because the
decomposition is what says whether the story is about the thing that is moving.

### A check that forces the architecture to move first

Adding a `.cube` reader meant `sapstudio-io` depending on `sapstudio-render`,
and both were layer three — so the layering check refused it. That refusal is
the whole value of having written the check: the alternative is quietly adding
a sideways edge because a file needed one, which is how a layering becomes a
drawing again.

The right answer was not to work around it. `io` is the format layer for every
domain crate, so it belongs above all of them, and it moved to its own layer
with `app` and the image above it. The document changed, then the manifest, in
that order — which is the order the check enforces and the reason it is worth
enforcing.

A rule that only fires when it is inconvenient is the only kind of rule that
does anything.

### A bounding box is not a shape

A test called `every_tetrahedron_is_reached_and_none_of_them_is_wrong` was
written for exactly one mutation: giving one of the six tetrahedra the wrong
vertices. That mutation passed it.

The test checked that each result lands within the range its cell's eight
corners span. A tetrahedron given the wrong vertices still interpolates between
corners of the same cell, so it stays inside that box — the check was true of
the bug as well as of the fix. What a wrong vertex set actually breaks is
*continuity*: the six tetrahedra meet on the planes where two fractions are
equal, both branches either side must agree on that plane, and a wrong vertex
set puts a step in the surface. In a grade that is a hard edge through a smooth
gradient, which is the artefact tetrahedral interpolation is chosen to avoid.

So the test that catches it walks a line across all three planes and asserts the
result never jumps. The lesson generalises past this case: **a containment check
is usually satisfied by the bug as well as the fix.** "Within range", "not
negative", "sums to one" — each is worth having and none of them distinguishes
a correct computation from a plausible wrong one. The distinguishing property is
almost always a *relationship* — continuity, monotonicity, an identity, an
exactly known value — rather than a bound.

The bounding-box test is kept, because it does catch the mutation that swaps two
orderings. Both are needed and neither is the other.

### A guard whose refusal nothing triggers

The automated fader is clamped to the fader's own travel, and removing the
clamp failed nothing. Not because the clamp is idle: without it an overshooting
curve produces a `FaderOutOfRange` *refusal* rather than a wrong number, so the
mutation would have been caught the moment any fixture asked for a value past
the end stop. None did. Every curve in the suite stayed inside the travel, so
the guard was never approached from the side it guards.

This is the same shape as a fixture too tidy to break, but it fails in a
quieter way: a missing clamp does not give a wrong answer, it gives an error,
and an error nothing triggers looks exactly like an error nothing needs. **A
guard is only checked by an input that reaches it**, and for a saturating guard
that means a fixture that deliberately exceeds the bound — here an ease
overshooting to 30.375 decibels on a fader that stops at 24.

### Check the status of the thing you are checking

Four separate times now, a check reported success because it was reading the
wrong thing.

A mutation was made and nothing failed — because the mutated code did not
compile. A hook was tested with a ninety-five character line of `x` — which the
prose rule deliberately exempts, since a line with no spaces is an unsplittable
URL. And `git commit | tail -3` inside an `if` reported success — because a
pipeline's status is its *last* command's, so the `if` was testing `tail`.

Each time the surface reading was "the guard does not work", and each time the
truth was "the check did not reach the guard". They fail identically from the
outside, which is what makes this worth its own heading.

A fourth, from a different direction. Four controls were run over the mixer
with `cargo test -p sapstudio-audio`, and one of them appeared to leave a test
untouched that it should plainly have broken. It had not: `cargo test` stops
after the first *test binary* that fails, so the later binaries never ran at
all. The mutation was real, the test was right, and the report was reading a
run that had stopped early.

`--no-fail-fast` is the flag, and printing how many tests *ran* is the check on
the check — the same discipline as reading test counts rather than failure
counts. A control over a crate with several test binaries is not a control
until every one of them has run.

The habit that catches all four: **say what you expect to see, then look for
that**, rather than looking for the absence of a complaint. An expected failure
should be seen failing, with its message read. An expected exit status should be
captured from the command that produces it, not from whatever the shell ran
last. And a test input should be checked against the rule's own exemptions
before it is trusted to trigger the rule.

### A control that does not compile is a control that did not run

Twice now a mutation has been made, the suite has come back with nothing
failing, and the honest reading was "the invariant is not checked" — when the
truth was that the mutated code never built. `warnings = "deny"` turns an
unused binding into an error, and a mutation that removes a use of something
removes it from a whole file.

So a control is only a control once its build has been seen to succeed. In
practice: keep the mutated code compiling (`let _ = &thing;` is enough), and
read the *test counts* rather than only the failures, because "no failures" and
"no tests ran" look identical through a filter.

That is not a hypothetical. One control in this table passed for a real reason —
the zero-decibel fast path in `Gain::factor` turned out not to be load-bearing —
and it would have been indistinguishable from a control that never ran, if the
build had not been checked.

## Fuzzing

Every parser has a target, and the list of parsers is long: the project file,
every container, every codec bitstream, subtitle formats, LUT files, fonts,
EDL, XML interchange, and the metadata inside all of them.

- Targets run in CI on every change to their parser, briefly, and on a schedule
  for longer.
- Every crash becomes a committed regression test with the input minimised.
- A corpus is committed and grows; it is never reset.
- A parser without a target does not ship (R-11.3).

## Golden output

Rendering is verified by hash, not by eye (R-4.10).

- A golden test names its project, its inputs, its settings, and the SHA-256 of
  its output.
- The same render on a different machine, at a different core count, in a
  different run order, must produce the same hash.
- A golden hash changes only in a commit that says why, shows the visual
  difference, and has been reviewed as an intended change.
- Reference frames are stored as OpenEXR or PNG beside the hash so that a
  failure can be looked at, not just counted.

`sapstudio_io::png` is what writes them: eight-bit grey, colour, or colour and
coverage, no interlacing, no palette, and no compression at all — a legal zlib
stream of DEFLATE *stored* blocks, so every byte of the file can be read by
hand. It refuses premultiplied coverage rather than guessing, because a PNG
stores straight colour and writing a premultiplied frame as though it were
straight is a picture darker than the one rendered, worst exactly at the edges
somebody is looking at.

It was checked against an independent decoder rather than only against itself:
Python's `zlib.decompress` and `zlib.crc32` read a capture back and the pixels
came out identical to the frame. A checksum verified only by the code that
wrote it is a checksum that agrees with itself.

**No reference is committed yet, and that is deliberate.** The only golden
render today is sixteen pixels by nine, which is not a picture and tells a
person nothing when it changes. The writer exists so the rule can be satisfied
the moment there is a frame worth looking at, and `SAP-03` is what makes one.

## Pull-request evidence

Every pull request states:

- the exact commands and CI workflows run;
- the scenario, transcript lines, or test names that establish the change;
- the negative control performed and the refusal it produced;
- any emulator, toolchain, or platform limitation that applies;
- the most credible failure the checks do not cover.

"None" is not an acceptable answer to the last one.

## Release evidence

A release publishes, from the release commit: the reproducible artefacts and
their digests, the vendored source of every dependency, the licence record, the
dependency manifest, every QEMU transcript, the golden hashes, the fuzz corpora
state, the benchmark results with their machine profile, and a plain statement
of what the release does not do.

A binary is not published unless those records match — the same rule Sapote
applies to its measured BusyBox profiles, for the same reason.
