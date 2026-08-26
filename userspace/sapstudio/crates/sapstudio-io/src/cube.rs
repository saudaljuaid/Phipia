// SPDX-License-Identifier: GPL-3.0-only
//! `.cube`: the file a colourist hands over.
//!
//! Adobe and Iridas's text format, and the one every grading system reads and
//! writes. A header of key-value lines, then `size³` triples of decimal
//! numbers, one per line:
//!
//! ```text
//! # a comment
//! TITLE "a look"
//! LUT_3D_SIZE 3
//! DOMAIN_MIN 0.0 0.0 0.0
//! DOMAIN_MAX 1.0 1.0 1.0
//! 0.0 0.0 0.0
//! 0.5 0.0 0.0
//! ...
//! ```
//!
//! # The traps, and what each one costs
//!
//! **Red varies fastest.** The innermost loop of the sample order is red, then
//! green, then blue. Reading it the other way transposes the cube — and a
//! transposed grade is not a crash or a garish mess, it is a *plausible*
//! picture with the wrong look, which is the worst kind of wrong for a format
//! to be. There is a test with a table that is asymmetric in every axis, so a
//! transposition cannot hide.
//!
//! **The numbers are decimal text.** Which means they can be read *exactly*:
//! `0.123456` is `123456/1000000` and nothing is lost. A reader that went
//! through a binary floating-point type would lose that on the way in, for no
//! reason, in a project that has no floating point anywhere else. So the
//! parser builds a [`Rational`] from the digits and converts once.
//!
//! **`DOMAIN_MIN` and `DOMAIN_MAX` are not always nought and one.** A table
//! authored for log footage may declare any input range, and applying it as
//! though the range were the unit interval silently applies the wrong look to
//! every pixel. This build reads the domain and refuses anything but the unit
//! interval by name, rather than ignoring the lines — which is the failure
//! mode a reader that skipped unknown keys would have.
//!
//! **Output values may leave nought to one, and must not be clamped.** A look
//! can send a highlight above white on purpose, and a reader that clamped on
//! the way in would quietly flatten it.
//!
//! **`LUT_1D_SIZE` is a different thing in the same file extension.** A
//! one-dimensional table is a per-channel curve, not a cube, and reading one
//! as the other would treat a curve's samples as a cube's and produce
//! nonsense. It is refused by name.
//!
//! **Scientific notation is refused rather than guessed at.** The format's
//! own description does not call for it, but files in the wild carry it for
//! very small values. Refusing by name says plainly that this is unimplemented
//! rather than mis-parsed, and adding it is a small change on the day a file
//! that needs it turns up — which is a better place to be than having read
//! `1e-3` as one.

use alloc::vec::Vec;

use sapstudio_core::Rational;
use sapstudio_render::Fixed;
use sapstudio_render::lut::{Colour, Lut3D, MAX_SIZE, MIN_SIZE};

use crate::status::{IoStatus, Result};

/// The most bytes one line may hold.
///
/// Two hundred and fifty-six, the same bound the edit decision list uses. A
/// line of three numbers and a key does not approach it, and the bound is what
/// stops a file with no newline in it from being read as one line the size of
/// the file (R-11.2).
pub const MAX_LINE_BYTES: usize = 256;

/// The most digits a number may carry after its point.
///
/// Nine, which is a resolution of one part in a thousand million — far past
/// what any table is authored at, and chosen so the numerator of the fraction
/// cannot leave an `i64` however many integer digits precede it.
pub const MAX_DECIMALS: usize = 9;

/// Read a table.
///
/// # Errors
///
/// Any [`IoStatus`]. Nothing is returned on a refusal.
pub fn parse(text: &str) -> Result<Lut3D> {
    let mut size: Option<usize> = None;
    let mut samples: Vec<Colour> = Vec::new();

    for line in text.lines() {
        if line.len() > MAX_LINE_BYTES {
            return Err(IoStatus::CubeLineTooLong);
        }
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let mut words = trimmed.split_whitespace();
        let Some(first) = words.next() else {
            continue;
        };

        match first {
            "TITLE" => {}
            "LUT_1D_SIZE" => return Err(IoStatus::CubeNotThreeDimensional),
            "LUT_3D_SIZE" => {
                if size.is_some() {
                    // Two sizes in one file is a file that does not know how
                    // big it is, and picking either would be a guess.
                    return Err(IoStatus::CubeSizeRepeated);
                }
                let declared = read_size(words.next().ok_or(IoStatus::CubeMalformed)?)?;
                samples
                    .try_reserve(declared * declared * declared)
                    .map_err(|_| IoStatus::OutOfMemory)?;
                size = Some(declared);
            }
            "DOMAIN_MIN" => check_domain(words, Rational::ZERO)?,
            "DOMAIN_MAX" => check_domain(words, Rational::ONE)?,
            _ => {
                // Anything else must be a sample, and a sample is three
                // numbers. A key this build does not know reaches here and
                // fails to parse as a number, which is the refusal it should
                // get: skipping unknown keys is how a reader ignores the one
                // line that changes what the file means.
                if size.is_none() {
                    return Err(IoStatus::CubeSampleBeforeSize);
                }
                samples.push(read_colour(first, &mut words)?);
            }
        }
    }

    let size = size.ok_or(IoStatus::CubeNoSize)?;
    Lut3D::new(size, samples).map_err(IoStatus::Render)
}

/// Write a table.
///
/// Nine decimal places, which is [`MAX_DECIMALS`] and enough to carry back
/// exactly what was read: the parser builds a fraction over a power of ten, so
/// a number that arrived with nine or fewer places leaves with the same digits.
///
/// # Errors
///
/// [`IoStatus::OutOfMemory`] and [`IoStatus::Render`].
pub fn write(table: &Lut3D) -> Result<alloc::string::String> {
    use core::fmt::Write as _;

    let mut out = alloc::string::String::new();
    let size = table.size();
    out.try_reserve(size * size * size * 36)
        .map_err(|_| IoStatus::OutOfMemory)?;
    writeln!(out, "LUT_3D_SIZE {size}").map_err(|_| IoStatus::OutOfMemory)?;
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                // Red fastest, matching the order the parser reads and every
                // other system writes.
                let sample = table.sample(red, green, blue).map_err(IoStatus::Render)?;
                for (channel, component) in sample.iter().enumerate() {
                    if channel > 0 {
                        out.push(' ');
                    }
                    write_fixed(&mut out, *component)?;
                }
                out.push('\n');
            }
        }
    }
    Ok(out)
}

/// Write one component as a decimal with [`MAX_DECIMALS`] places.
fn write_fixed(out: &mut alloc::string::String, value: Fixed) -> Result<()> {
    use core::fmt::Write as _;

    let scale = 10_i64.pow(u32::try_from(MAX_DECIMALS).map_err(|_| IoStatus::CubeMalformed)?);
    let ratio = value.to_rational().map_err(|_| IoStatus::CubeMalformed)?;
    let scaled = ratio
        .checked_mul(Rational::new(scale, 1).map_err(|_| IoStatus::CubeMalformed)?)
        .map_err(|_| IoStatus::CubeMalformed)?;
    let rounded = scaled.floor().map_err(|_| IoStatus::CubeMalformed)?;
    let (sign, magnitude) = if rounded < 0 {
        ("-", -rounded)
    } else {
        ("", rounded)
    };
    let whole = magnitude / scale;
    let fraction = magnitude % scale;
    let places = MAX_DECIMALS;
    write!(out, "{sign}{whole}.{fraction:0places$}").map_err(|_| IoStatus::OutOfMemory)
}

/// A declared side, checked against what a table may hold.
fn read_size(word: &str) -> Result<usize> {
    let mut value = 0_usize;
    if word.is_empty() {
        return Err(IoStatus::CubeMalformed);
    }
    for byte in word.bytes() {
        let digit = match byte {
            b'0'..=b'9' => usize::from(byte - b'0'),
            _ => return Err(IoStatus::CubeMalformed),
        };
        value = value
            .checked_mul(10)
            .and_then(|held| held.checked_add(digit))
            .ok_or(IoStatus::CubeSizeUnsupported)?;
    }
    if !(MIN_SIZE..=MAX_SIZE).contains(&value) {
        return Err(IoStatus::CubeSizeUnsupported);
    }
    Ok(value)
}

/// A domain line, which this build accepts only at the unit interval.
fn check_domain<'a>(words: impl Iterator<Item = &'a str>, expected: Rational) -> Result<()> {
    let mut seen = 0;
    for word in words {
        if read_number(word)? != expected {
            // Refused rather than ignored. A table authored for another input
            // range applied as though it were the unit interval is the wrong
            // look on every pixel, silently, and a reader that skipped the
            // line it did not handle would do exactly that.
            return Err(IoStatus::CubeDomainUnsupported);
        }
        seen += 1;
    }
    if seen != 3 {
        return Err(IoStatus::CubeMalformed);
    }
    Ok(())
}

/// One sample, as three numbers.
fn read_colour<'a>(first: &str, rest: &mut impl Iterator<Item = &'a str>) -> Result<Colour> {
    let red = read_number(first)?;
    let green = read_number(rest.next().ok_or(IoStatus::CubeMalformed)?)?;
    let blue = read_number(rest.next().ok_or(IoStatus::CubeMalformed)?)?;
    if rest.next().is_some() {
        // A fourth number on a sample line is a file this reader does not
        // understand, and taking the first three would be reading part of a
        // record as the whole of it.
        return Err(IoStatus::CubeMalformed);
    }
    Ok([
        Fixed::from_rational(red).map_err(|_| IoStatus::CubeOutOfRange)?,
        Fixed::from_rational(green).map_err(|_| IoStatus::CubeOutOfRange)?,
        Fixed::from_rational(blue).map_err(|_| IoStatus::CubeOutOfRange)?,
    ])
}

/// A decimal number, read exactly.
///
/// `0.123456` becomes `123456/1000000`, which is what it says. Going through a
/// binary floating-point type would lose that on the way in — for no reason,
/// in a project with no floating point anywhere else.
fn read_number(word: &str) -> Result<Rational> {
    let (negative, digits) = match word.as_bytes().first() {
        Some(b'-') => (true, &word[1..]),
        Some(b'+') => (false, &word[1..]),
        _ => (false, word),
    };
    if digits.is_empty() {
        return Err(IoStatus::CubeMalformed);
    }

    let mut numerator = 0_i64;
    let mut denominator = 1_i64;
    let mut past_point = false;
    let mut decimals = 0;
    for byte in digits.bytes() {
        match byte {
            b'.' if !past_point => past_point = true,
            b'e' | b'E' => return Err(IoStatus::CubeExponentUnsupported),
            b'0'..=b'9' => {
                if past_point {
                    decimals += 1;
                    if decimals > MAX_DECIMALS {
                        return Err(IoStatus::CubeTooPrecise);
                    }
                    denominator = denominator
                        .checked_mul(10)
                        .ok_or(IoStatus::CubeTooPrecise)?;
                }
                numerator = numerator
                    .checked_mul(10)
                    .and_then(|held| held.checked_add(i64::from(byte - b'0')))
                    .ok_or(IoStatus::CubeOutOfRange)?;
            }
            // A second point falls here, as does any byte that is not a
            // digit: both are a number this reader cannot make sense of, and
            // giving them separate refusals would be inventing a distinction
            // a caller cannot act on differently.
            _ => return Err(IoStatus::CubeMalformed),
        }
    }
    if negative {
        numerator = numerator.checked_neg().ok_or(IoStatus::CubeOutOfRange)?;
    }
    Rational::new(numerator, denominator).map_err(IoStatus::Time)
}
