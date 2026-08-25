// SPDX-License-Identifier: GPL-3.0-only
//! The uncompressed mezzanine: what it accepts, and everything it refuses.

use sapstudio_core::{Rational, Timebase};
use sapstudio_io::IoStatus;
use sapstudio_io::sprw::{self, FORMAT_VERSION, HEADER_BYTES, MAGIC, Reel};
use sapstudio_media::{
    AlphaState, ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
    TestPattern,
};

/// What a format's alpha tag must be for a description to be accepted at all.
///
/// A format either carries an alpha channel or it does not, and a description
/// must say which association applies exactly when it does.
fn alpha_for(format: PixelFormat) -> Option<AlphaState> {
    if format.has_alpha() {
        Some(AlphaState::Straight)
    } else {
        None
    }
}

fn description(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        alpha_for(format),
    )
    .expect("a description")
}

/// A short reel of test patterns.
fn sample() -> Reel {
    let description = description(16, 9, PixelFormat::Rgb8);
    let frames = std::vec![
        TestPattern::Bars.render(description).expect("a frame"),
        TestPattern::Ramp.render(description).expect("a frame"),
        TestPattern::Flat { value: 77 }
            .render(description)
            .expect("a frame"),
    ];
    Reel::new(Timebase::NTSC_FILM, frames).expect("a reel")
}

#[test]
fn a_reel_survives_a_round_trip() {
    let reel = sample();
    let file = sprw::encode(&reel).expect("an encoding");
    let decoded = sprw::decode(&file).expect("a decoding");
    assert_eq!(decoded, reel);
    assert_eq!(decoded.timebase(), Timebase::NTSC_FILM);
    assert_eq!(decoded.len(), 3);
    for (before, after) in reel.frames().iter().zip(decoded.frames()) {
        assert_eq!(before.digest(), after.digest(), "frame for frame");
    }
}

#[test]
fn the_encoding_is_canonical() {
    let first = sprw::encode(&sample()).expect("an encoding");
    let second = sprw::encode(&sprw::decode(&first).expect("a decoding")).expect("an encoding");
    assert_eq!(first, second);
}

#[test]
fn the_file_is_exactly_as_large_as_it_should_be() {
    let reel = sample();
    let file = sprw::encode(&reel).expect("an encoding");
    let frame_bytes = reel.description().packed_bytes().expect("a size");
    assert_eq!(file.len(), HEADER_BYTES + frame_bytes * reel.len());
    assert_eq!(&file[..4], &MAGIC);
}

#[test]
fn every_format_and_description_round_trips() {
    let cases: [(PixelFormat, Option<ChromaSiting>, ColourDescription); 4] = [
        (PixelFormat::Rgba8, None, ColourDescription::srgb_full()),
        (PixelFormat::Gray8, None, ColourDescription::srgb_full()),
        (
            PixelFormat::Yuv420p8,
            Some(ChromaSiting::Centre),
            ColourDescription::bt709_limited(),
        ),
        (
            PixelFormat::Yuv444p8,
            None,
            ColourDescription::bt709_limited(),
        ),
    ];
    for (format, siting, colour) in cases {
        let described = FrameDescription::new(
            Geometry::new(8, 6).expect("a geometry"),
            format,
            colour,
            siting,
            alpha_for(format),
            Rational::new(16, 11).expect("a ratio"),
        )
        .expect("a description");
        let frame = Frame::blank(described).expect("a frame");
        let reel = Reel::new(Timebase::PAL_25, std::vec![frame]).expect("a reel");
        let file = sprw::encode(&reel).expect("an encoding");
        assert_eq!(sprw::decode(&file).expect("a decoding"), reel, "{format:?}");
    }
}

#[test]
fn frames_described_differently_are_not_one_reel() {
    let first = Frame::blank(description(8, 8, PixelFormat::Gray8)).expect("a frame");
    let second = Frame::blank(description(8, 4, PixelFormat::Gray8)).expect("a frame");
    assert_eq!(
        Reel::new(Timebase::PAL_25, std::vec![first, second]),
        Err(IoStatus::ReelDescriptionMismatch),
        "a file whose frames change shape halfway through is two files"
    );
}

#[test]
fn a_reel_with_no_frames_cannot_be_built_or_read() {
    assert_eq!(
        Reel::new(Timebase::PAL_25, std::vec![]),
        Err(IoStatus::EmptyReel)
    );

    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[56..64].copy_from_slice(&0_u64.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::EmptyReel));
}

#[test]
fn a_file_that_is_not_a_reel_is_refused() {
    assert_eq!(sprw::decode(b""), Err(IoStatus::TruncatedHeader));
    assert_eq!(sprw::decode(&[0; 95]), Err(IoStatus::TruncatedHeader));

    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[1] = b'X';
    assert_eq!(sprw::decode(&file), Err(IoStatus::NotAReel));
}

#[test]
fn a_future_version_is_refused_by_number() {
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[4..6].copy_from_slice(&(FORMAT_VERSION + 1).to_le_bytes());
    assert_eq!(
        sprw::decode(&file),
        Err(IoStatus::UnsupportedVersion(FORMAT_VERSION + 1))
    );
}

#[test]
fn an_undefined_tag_is_refused_rather_than_defaulted() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for (offset, name) in [
        (16_usize, "pixel format"),
        (17, "primaries"),
        (18, "transfer"),
        (19, "matrix"),
        (20, "range"),
        (21, "siting"),
    ] {
        let mut mutated = file.clone();
        mutated[offset] = 200;
        assert!(
            sprw::decode(&mutated).is_err(),
            "an undefined {name} tag was accepted"
        );
    }
}

#[test]
fn a_frame_count_that_disagrees_with_the_payload_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");

    let mut more = file.clone();
    more[56..64].copy_from_slice(&4_u64.to_le_bytes());
    assert_eq!(sprw::decode(&more), Err(IoStatus::TruncatedPayload));

    let mut fewer = file.clone();
    fewer[56..64].copy_from_slice(&2_u64.to_le_bytes());
    assert_eq!(sprw::decode(&fewer), Err(IoStatus::TrailingBytes));
}

#[test]
fn a_frame_count_no_payload_could_hold_is_refused_before_anything_is_allocated() {
    // The hostile case: a header claiming four billion frames. It must be
    // refused by the bound, not by the allocator running out (R-11.2).
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[56..64].copy_from_slice(&u64::MAX.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::TooMany));

    file[56..64].copy_from_slice(&100_000_u64.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::TooMany));
}

#[test]
fn a_geometry_the_format_cannot_express_is_refused() {
    // 4:2:0 halves both dimensions, so an odd width has no chroma plane.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[16] = 4; // claim 4:2:0
    file[21] = 1; // and a siting, so that is not what fails
    assert!(sprw::decode(&file).is_err());
}

#[test]
fn every_single_byte_change_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for index in 0..file.len() {
        for replacement in [0x00_u8, 0x01, 0x55, 0xFF] {
            if file[index] == replacement {
                continue;
            }
            let mut mutated = file.clone();
            mutated[index] = replacement;
            assert!(
                sprw::decode(&mutated).is_err(),
                "byte {index} changed to {replacement:#04x} was accepted"
            );
        }
    }
}

#[test]
fn every_prefix_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for length in 0..file.len() {
        assert!(
            sprw::decode(&file[..length]).is_err(),
            "a reel truncated to {length} bytes was accepted"
        );
    }
    assert!(sprw::decode(&file).is_ok());
}

/// xorshift64*, so a failure is a seed rather than an anecdote.
struct Generator(u64);

impl Generator {
    fn next(&mut self) -> u64 {
        let mut state = self.0;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        self.0 = state;
        state.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
}

#[test]
fn garbage_is_refused_and_never_anything_worse() {
    let mut generator = Generator(0xFEED_FACE_CAFE_BEEF);
    for _ in 0..2000 {
        let length = usize::try_from(generator.next() % 400).unwrap_or(0);
        let mut bytes: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            bytes.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        assert!(sprw::decode(&bytes).is_err(), "random bytes were accepted");
    }
}

#[test]
fn the_alpha_association_survives_the_file() {
    // A mezzanine that dropped this would hand the next stage samples whose
    // meaning it had to guess, and guessing wrong is a dark fringe.
    for state in [AlphaState::Straight, AlphaState::Premultiplied] {
        let described = FrameDescription::square(
            Geometry::new(8, 6).expect("a geometry"),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            None,
            Some(state),
        )
        .expect("a description");
        let reel = Reel::new(
            Timebase::PAL_25,
            std::vec![Frame::blank(described).expect("a frame")],
        )
        .expect("a reel");
        let file = sprw::encode(&reel).expect("an encoding");
        assert_eq!(file[22], if state == AlphaState::Straight { 1 } else { 2 });
        let back = sprw::decode(&file).expect("a decoding");
        assert_eq!(back.description().alpha(), Some(state));
        assert_eq!(back, reel);
    }
}

#[test]
fn changing_the_alpha_byte_changes_the_file() {
    // The header is digested, so this must be caught rather than silently
    // reinterpreted — the gap the byte sweep found once already.
    let described = FrameDescription::square(
        Geometry::new(8, 6).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let reel = Reel::new(
        Timebase::PAL_25,
        std::vec![Frame::blank(described).expect("a frame")],
    )
    .expect("a reel");
    let mut file = sprw::encode(&reel).expect("an encoding");
    file[22] = 2;
    assert_eq!(
        sprw::decode(&file),
        Err(IoStatus::DigestMismatch),
        "premultiplied is not what this file says it holds"
    );
}

#[test]
fn an_unknown_alpha_tag_is_refused() {
    // The header is parsed before the digest is checked, so this is the tag
    // reader refusing rather than the digest catching it. Both must hold, and
    // a test that would accept either proves neither.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[22] = 3;
    assert_eq!(sprw::decode(&file), Err(IoStatus::UnknownColourTag(3)));
}

#[test]
fn a_version_one_file_is_not_read_as_version_two() {
    // Version one had two reserved bytes where version two keeps the alpha
    // association. Reading one as the other would silently call every frame
    // straight, so the version number is checked before any field is.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[4] = 1;
    file[5] = 0;
    assert_eq!(sprw::decode(&file), Err(IoStatus::UnsupportedVersion(1)));
}

#[test]
fn an_alpha_tag_a_format_cannot_carry_is_refused_even_with_a_sound_digest() {
    // A corrupt file is caught by the digest. This is the other case: a file
    // that is internally consistent and still describes something that is not
    // a frame — an alpha association on a format with no alpha channel. The
    // digest is recomputed so that it cannot be what does the refusing.
    let file = sprw::encode(&sample()).expect("an encoding");
    let mut forged = file.clone();
    assert_eq!(forged[16], 2, "the sample reel is Rgb8, which has no alpha");
    forged[22] = 1;
    let mut hasher = sapstudio_core::Sha256::new();
    hasher.update(&forged[..sprw::DESCRIBED_BYTES]);
    hasher.update(&forged[HEADER_BYTES..]);
    let digest = hasher.finish();
    forged[sprw::DESCRIBED_BYTES..HEADER_BYTES].copy_from_slice(digest.bytes());

    assert_ne!(forged, file, "the forgery must differ from the original");
    assert_eq!(
        sprw::decode(&forged),
        Err(IoStatus::Media(sapstudio_media::MediaStatus::AlphaMismatch)),
        "a sound digest over a description that cannot exist is still refused"
    );
}
