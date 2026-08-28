// SPDX-License-Identifier: GPL-3.0-only
//! The slate's whole output, pinned.
//!
//! This is a golden transcript in the sense R-14.4 means: the output is not
//! inspected for plausibility, it is compared. Every label in it is exact
//! drop-frame timecode over a ten-minute asset, so a single wrong frame
//! anywhere in the time arithmetic, the model, or the report changes a
//! character here and fails this test by name.
//!
//! The three digests are the saved project file's, the encoded reel's, and a
//! rendered frame's — so this one string pins two formats, the test patterns
//! behind them, and the whole render path: the layer stack, the plan, the
//! graph, the compositor and the pool. Change a field, a width, an order, or a
//! pixel and this test says so.
//!
//! The render hash is what `docs/VERIFICATION.md` asks every render to carry
//! and what nothing here had until the picture existed. It names its project,
//! its instant and its description, so it moves when any of the three does —
//! or when the arithmetic behind them does.
//!
//! `picture red` is 73, and every step of that is somewhere else in the
//! program. At frame 12 of a 24-frame rise the upper track sits at half
//! opacity; fading a premultiplied layer scales its coverage too, so the top
//! is red 64 at coverage 64; and `over` works in linear light, where
//! `0.0513 + 0.0212·(1 − 64/255)` encodes back to 73. Adding code values would
//! give something else entirely, which is the mistake an earlier version of
//! the comment in the slate actually made.
//!
//! The fade is why the *instant* is pinned at all. Without it both clips run
//! the whole span, every instant renders identically, and moving the playhead
//! a frame breaks nothing — which a negative control demonstrated before this
//! line existed.
//!
//! The saved size moved 247 → 255 → 263 → 266, and on to 301 and now 313. The
//! first two steps are eight bytes each: two tracks times the four-byte
//! keyframe count of an automation lane with nothing on it. The third is
//! three: one flag byte per clip, and the slate lays three clips. The last is
//! twelve: four bytes per clip for the keyframe count of a grade strength
//! nobody set, measured against the previous commit in a worktree at one,
//! three and seven clips — 203 → 207, 267 → 279, 395 → 423 — which is what
//! says the step is per clip rather than per file.
//!
//! Each difference is *evidence* that nothing else in the format moved, rather
//! than a number accepted because the test asked for one. A step of any other
//! size would mean looking, not editing.
//!
//! And `picture digest` did **not** move for this one, which is the other half
//! of the same evidence: no clip here carries a grade, so nothing the strength
//! reaches is in the picture. A commit that moved both numbers would need two
//! accounts, and this one has one.
//!
//! The turn lane moved the file's digest and **not** its length, and that was
//! checked rather than assumed, because "only the version byte changed" is the
//! easiest thing to believe and the easiest to be wrong about. Encoding a
//! motionless project under both versions gives two files of 207 bytes that
//! differ in exactly one byte, at offset four. A project that *does* carry a
//! motion grows by four — the turn's own keyframe count of nought — which is
//! why the slate, which animates nothing, pays nothing.
//!
//! The anchor did the same again, and was checked the same way: 207 bytes
//! under versions twenty-two and twenty-three, one byte apart at offset four,
//! and **thirty-two** bytes on any clip that carries a transform — two
//! rationals for the pivot. The slate frames nothing, so once more only its
//! digest moved. Two consecutive commits where the length did not change is
//! exactly when it is worth running the comparison rather than recognising
//! the pattern.
//!
//! Markers took it to 317: **four bytes a sequence** for a count of nought,
//! measured at one sequence (207 → 211) and again at two (227 → 235), which is
//! what says the step is per sequence rather than per file. The slate lays one
//! sequence and writes no notes on it. A marker that says something costs
//! twelve bytes plus its own text — eight for the instant, four for the
//! length — measured at one and at three.

use sapstudio_abi::seam::{Console, Result, SeamStatus};
use sapstudio_app::{EXIT_FAILURE, EXIT_SUCCESS, run};

/// What the slate must print, byte for byte.
const GOLDEN: &str = "\
SapStudio slate

timebase       30000/1001
media length   00:10:00;00

after the cut
  V1  00:00:00;00  00:00:58;10  clip  src 00:00:11;18
  V1  00:00:58;10  00:01:28;12  clip  src 00:02:46;24
  A1  00:00:00;00  00:01:40;00  clip  src 00:00:10;00
duration       00:01:40;00

undone         7 edits
tracks now     0
redone         7 edits
restored       true

saved          317
digest         D6245B0168B3C9EF53D15D0AAC948B7FABEE496F77E49583FDE65E69F15EC449
round trip     true

reel frames    3
reel bytes     528
reel digest    53F707D400833A4F2492DCC4A819B30C78367758C69FEA8E68AF28FB7A85D800
reel matches   true
pool frames    2
pool bytes     288
pool evictions 1

picture size   576
picture digest 74202C026AAA084C24490F8ED394BF021782A396882F2632B51949E8A20F9D20
picture red    98
picture alpha  255

slate complete
";

/// A console that keeps what it was given, so a test can read it back.
#[derive(Default)]
struct Buffer(std::vec::Vec<u8>);

impl Console for Buffer {
    fn write(&mut self, bytes: &[u8]) -> Result<()> {
        self.0.extend_from_slice(bytes);
        Ok(())
    }
}

/// A console that refuses after a given number of calls.
struct Failing {
    remaining: usize,
}

impl Console for Failing {
    fn write(&mut self, _: &[u8]) -> Result<()> {
        if self.remaining == 0 {
            return Err(SeamStatus::Refused);
        }
        self.remaining -= 1;
        Ok(())
    }
}

#[test]
fn the_slate_prints_its_golden_transcript() {
    let mut buffer = Buffer::default();
    assert_eq!(run(&mut buffer), EXIT_SUCCESS);
    let transcript = std::string::String::from_utf8(buffer.0).expect("the report is text");
    assert_eq!(transcript, GOLDEN);
}

#[test]
fn the_slate_is_deterministic() {
    // R-4.1 in the smallest form the application currently has: the same run
    // twice produces the same bytes. Nothing in it may consult a clock, an
    // address, or an allocation order.
    let mut first = Buffer::default();
    let mut second = Buffer::default();
    assert_eq!(run(&mut first), EXIT_SUCCESS);
    assert_eq!(run(&mut second), EXIT_SUCCESS);
    assert_eq!(first.0, second.0);
}

#[test]
fn a_console_refusal_is_reported_rather_than_ignored() {
    // Formatting cannot carry a seam refusal out through core::fmt, so the
    // writer records it. If that plumbing were dropped, the slate would
    // silently claim success on a console that wrote nothing.
    let mut failing = Failing { remaining: 3 };
    assert_eq!(run(&mut failing), EXIT_FAILURE);
}
