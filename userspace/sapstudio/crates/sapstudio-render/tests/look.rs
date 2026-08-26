// SPDX-License-Identifier: GPL-3.0-only
//! Applying a lookup table to a frame.
//!
//! The cube's own tests are about interpolation. These are about the three
//! decisions that have to be made before a single sample reaches it: which
//! encoding the table was authored for, what happens to coverage, and what a
//! table does to the channels it was not given.

use sapstudio_core::{Fixed, Rational};
use sapstudio_media::colour::{Range, TransferFunction};
use sapstudio_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
};
use sapstudio_render::lut::{Colour, Interpolation, Lut3D};
use sapstudio_render::{Look, RenderStatus};

fn described(format: PixelFormat, colour: ColourDescription) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        format,
        colour,
        None,
        if format.has_alpha() {
            Some(AlphaState::Straight)
        } else {
            None
        },
    )
    .expect("a description")
}

/// A fixed-point value from a fraction.
fn at(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

/// A frame whose pixels are the bytes given, repeated to fill it.
fn frame(description: &FrameDescription, pattern: &[u8]) -> Frame {
    let wanted = description.packed_bytes().expect("a size");
    let bytes: std::vec::Vec<u8> = pattern.iter().copied().cycle().take(wanted).collect();
    Frame::from_packed(*description, &bytes).expect("a frame")
}

/// A table that swaps red and blue, leaving green and the diagonal alone.
///
/// Neutral on its own diagonal, so a grey must survive it, and violently
/// non-neutral everywhere else, so anything that reaches the table wrongly
/// shows.
fn swap_red_and_blue(size: usize) -> Lut3D {
    let last = i64::try_from(size - 1).expect("a size");
    let mut samples = std::vec::Vec::new();
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let (r, g, b) = (
                    i64::try_from(red).expect("an index"),
                    i64::try_from(green).expect("an index"),
                    i64::try_from(blue).expect("an index"),
                );
                samples.push([at(b, last), at(g, last), at(r, last)] as Colour);
            }
        }
    }
    Lut3D::new(size, samples).expect("a table")
}

#[test]
fn an_identity_look_leaves_a_frame_alone() {
    // Not "nearly": every sample must come back the code value it went in as.
    // A table that changes nothing, applied in the space it was authored for,
    // has nothing to round — the lattice reproduces exactly and the
    // normalisation and quantisation are each other's inverse on a code value.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let original = frame(&description, &[0, 17, 128, 200, 255, 64]);
    let look = Look::new(
        Lut3D::identity(17).expect("a table"),
        colour,
        Interpolation::Tetrahedral,
    );
    let after = look.apply(&original).expect("a frame");
    assert_eq!(
        after.to_packed().expect("bytes"),
        original.to_packed().expect("bytes"),
        "an identity table moved a sample"
    );
    assert_eq!(after.description(), original.description());
}

#[test]
fn a_look_changes_the_numbers_and_not_what_they_mean() {
    // The output is described exactly as the input was. A table authored for a
    // display encoding takes display-encoded values to display-encoded values,
    // and claiming the output was in some other space would be a conversion
    // nobody performed.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let original = frame(&description, &[200, 40, 10]);
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    let after = look.apply(&original).expect("a frame");

    assert_eq!(
        after.description(),
        original.description(),
        "the description moved"
    );
    let bytes = after.to_packed().expect("bytes");
    assert_eq!(
        &bytes[..3],
        &[10, 40, 200],
        "red and blue did not swap, so the table did not reach the pixel"
    );
}

#[test]
fn a_grey_survives_a_look_that_is_neutral_on_its_diagonal() {
    // The property the whole interpolation choice was made for, end to end
    // through a frame rather than in the cube on its own. Every grey in, every
    // grey out.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    for level in [0_u8, 1, 37, 128, 199, 254, 255] {
        let grey = frame(&description, &[level, level, level]);
        let after = look.apply(&grey).expect("a frame");
        let bytes = after.to_packed().expect("bytes");
        assert_eq!(
            bytes[0],
            bytes[1],
            "a grey at {level} came out tinted: {:?}",
            &bytes[..3]
        );
        assert_eq!(bytes[1], bytes[2], "a grey at {level} came out tinted");
    }
}

#[test]
fn a_frame_in_another_encoding_is_refused() {
    // A show LUT built for a camera's log curve, applied to display-referred
    // pictures, is the wrong look on every pixel — and nothing crashes, and
    // there is nothing to compare against. So the look carries the encoding it
    // was authored for and a frame that does not match is refused rather than
    // fed in anyway.
    let authored = ColourDescription::srgb_full();
    let look = Look::new(
        Lut3D::identity(5).expect("a table"),
        authored,
        Interpolation::Tetrahedral,
    );

    let mut other = authored;
    other.transfer = TransferFunction::Gamma22;
    let description = described(PixelFormat::Rgb8, other);
    assert_eq!(
        look.apply(&frame(&description, &[128, 128, 128]))
            .map(|_| ()),
        Err(RenderStatus::LookSpaceMismatch)
    );

    // A different range is a different encoding too: the same code value means
    // a different amount of light.
    let mut limited = authored;
    limited.range = Range::Limited;
    let described_limited = described(PixelFormat::Rgb8, limited);
    assert_eq!(
        look.apply(&frame(&described_limited, &[128, 128, 128]))
            .map(|_| ()),
        Err(RenderStatus::LookSpaceMismatch)
    );
}

#[test]
fn premultiplied_coverage_is_refused_rather_than_quietly_undone() {
    // A table is a non-linear function applied per pixel. On premultiplied
    // samples that computes f(ac) where the answer wanted is a·f(c), and those
    // agree only when f is linear or a is one — and full coverage is exactly
    // what a test made of opaque bars would cover, which is how the same
    // mistake reached the conversion path once already.
    //
    // Unpremultiplying here would be worse than refusing: it is lossy, so
    // doing it silently spends a real quantity of the caller's picture on a
    // step the caller did not ask for.
    let colour = ColourDescription::srgb_full();
    let straight = described(PixelFormat::Rgba8, colour);
    let look = Look::new(
        Lut3D::identity(5).expect("a table"),
        colour,
        Interpolation::Tetrahedral,
    );
    assert!(
        look.apply(&frame(&straight, &[10, 20, 30, 128])).is_ok(),
        "straight coverage was refused"
    );

    let premultiplied = FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        PixelFormat::Rgba8,
        colour,
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    assert_eq!(
        look.apply(&frame(&premultiplied, &[10, 20, 30, 128]))
            .map(|_| ()),
        Err(RenderStatus::LookPremultiplied)
    );
}

#[test]
fn coverage_is_carried_through_untouched() {
    // The last time something in this crate wrote a constant into an alpha
    // byte instead of carrying it, every keyed frame that went through came
    // out a solid rectangle — and the test that should have caught it used
    // opaque bars, which have nothing to lose. So this one is keyed, at four
    // different coverages, and the table is one that changes the colour so a
    // pass-through cannot be mistaken for the table doing nothing.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgba8, colour);
    let original = frame(
        &description,
        &[
            200, 40, 10, 0, 200, 40, 10, 90, 200, 40, 10, 200, 200, 40, 10, 255,
        ],
    );
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    let after = look.apply(&original).expect("a frame");

    let before = original.to_packed().expect("bytes");
    let bytes = after.to_packed().expect("bytes");
    for pixel in 0..8 {
        assert_eq!(
            bytes[pixel * 4 + 3],
            before[pixel * 4 + 3],
            "pixel {pixel} had its coverage rewritten"
        );
        assert_ne!(
            &bytes[pixel * 4..pixel * 4 + 3],
            &before[pixel * 4..pixel * 4 + 3],
            "pixel {pixel} was not touched at all, so this proves nothing"
        );
    }
}

#[test]
fn a_format_that_is_not_red_green_blue_is_refused() {
    // A table maps three colour channels to three. A luma-chroma frame needs
    // the matrix taken out of it first, and a grey frame has nowhere to put a
    // colour — each of those is a named step of its own rather than something
    // to do on the way past.
    //
    // Each look is built for the *same* description as the frame it is given,
    // so what refuses is the format check and not the encoding check. A test
    // that let the encodings differ would pass for the wrong reason and would
    // go on passing if the format check were deleted.
    let grey_colour = ColourDescription::srgb_full();
    let video_colour = ColourDescription::bt709_limited();
    for (format, colour) in [
        (PixelFormat::Gray8, grey_colour),
        (PixelFormat::Yuv444p8, video_colour),
        (PixelFormat::Yuv422p8, video_colour),
    ] {
        let description = FrameDescription::square(
            Geometry::new(4, 2).expect("a geometry"),
            format,
            colour,
            if format.is_subsampled() {
                Some(sapstudio_media::ChromaSiting::Centre)
            } else {
                None
            },
            None,
        )
        .expect("a description");
        let look = Look::new(
            Lut3D::identity(5).expect("a table"),
            colour,
            Interpolation::Tetrahedral,
        );
        assert_eq!(
            look.apply(&Frame::blank(description).expect("a frame"))
                .map(|_| ()),
            Err(RenderStatus::LookNotRgb),
            "{format:?} was accepted"
        );
    }
}

#[test]
fn a_look_is_the_same_look_every_time() {
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgba8, colour);
    let original = frame(&description, &[3, 130, 250, 77, 199, 8, 44, 255]);
    let look = Look::new(swap_red_and_blue(9), colour, Interpolation::Tetrahedral);
    let first = look.apply(&original).expect("a frame");
    let second = look.apply(&original).expect("a frame");
    assert_eq!(first.digest(), second.digest());
}
