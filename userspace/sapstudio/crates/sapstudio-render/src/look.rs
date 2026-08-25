// SPDX-License-Identifier: GPL-3.0-only
//! Applying a lookup table to a frame.
//!
//! [`crate::lut`] holds the cube and knows how to interpolate inside it. This
//! is the part that decides *what to feed it*, and that is the whole of the
//! difficulty: a table is a sampled function of three numbers, and a frame is
//! a great many triples of numbers in some particular encoding, and which
//! triples reach the table is a decision nobody can make from the table alone.
//!
//! # In what space
//!
//! A look is authored **for** an encoding. A show LUT built for a camera's log
//! curve expects log-encoded values; a display LUT expects display-referred
//! ones. The cube itself carries no record of which — it is 35,937 triples —
//! so applying one to the wrong encoding is the wrong look on every pixel,
//! with nothing crashing and nothing to compare against.
//!
//! So a [`Look`] carries the description its table was authored for, and a
//! frame that does not match is refused by name. Converting the frame first is
//! the caller's decision to make and to name (R-1.3), and
//! [`crate::convert::convert`] is how it is made.
//!
//! # Not in linear light, and that is not an inconsistency
//!
//! The compositor works in linear light and nowhere else, because `over` is a
//! statement about how much light reaches the eye and that sentence is only
//! true of light. A lookup table is the opposite case: it is a sampled
//! function of *code values*, authored by somebody looking at code values, and
//! decoding to light before feeding it would send the table inputs it was
//! never sampled at.
//!
//! Both rules come from the same place — apply an operation in the space its
//! definition is written in — and they point in different directions because
//! the two definitions do.
//!
//! # Straight alpha only
//!
//! A table is a non-linear function applied per pixel. On premultiplied
//! samples that computes `f(αc)` where the answer wanted is `α·f(c)`, and
//! those are equal only when `f` is linear or `α` is one. Full coverage is
//! exactly the case a test made of opaque bars would cover, which is how the
//! same mistake reached the conversion path once already.
//!
//! Unpremultiplying here would be worse than refusing: it is lossy — exact at
//! full coverage, within two code values at half, and total at zero — so doing
//! it silently would spend a real quantity of the caller's picture on a step
//! the caller did not ask for. [`crate::composite::unpremultiply`] is the name
//! of that step, and asking for it is the caller's business.

use alloc::vec::Vec;

use sapstudio_core::{Digest, Sha256};
use sapstudio_media::colour::{AlphaState, ColourDescription, MatrixCoefficients};
use sapstudio_media::{Frame, FrameDescription, Geometry, PixelFormat};

use crate::convert::{normalise, quantise};
use crate::lut::{Interpolation, Lut3D};
use crate::status::{RenderStatus, Result};

/// A lookup table, and what it was authored for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Look {
    /// The cube.
    table: Lut3D,
    /// The encoding the cube's inputs are in.
    expects: ColourDescription,
    /// How to interpolate between its samples.
    interpolation: Interpolation,
}

impl Look {
    /// Pair a table with the encoding it was authored for.
    #[must_use]
    pub const fn new(
        table: Lut3D,
        expects: ColourDescription,
        interpolation: Interpolation,
    ) -> Self {
        Self {
            table,
            expects,
            interpolation,
        }
    }

    /// The cube.
    #[must_use]
    pub const fn table(&self) -> &Lut3D {
        &self.table
    }

    /// The encoding this look expects its input in.
    #[must_use]
    pub const fn expects(&self) -> ColourDescription {
        self.expects
    }

    /// How this look interpolates between its samples.
    #[must_use]
    pub const fn interpolation(&self) -> Interpolation {
        self.interpolation
    }

    /// What this look *is*, as a digest over everything that changes what it
    /// does.
    ///
    /// The samples, the encoding it expects, and the interpolation — because
    /// two of those three are as capable of changing every pixel as the cube
    /// is. A digest over the samples alone would make a table read
    /// tetrahedrally and the same table read trilinearly the same look, and a
    /// cache holding one would answer for the other.
    ///
    /// This is what a project stores and what a render graph names a grade by,
    /// for the same reason media is named by content: the same look in two
    /// projects is the same look, and a handle would cache it twice and could
    /// not tell that the file behind it had been swapped.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Media`] if the encoding cannot be written down, which
    /// no description this type accepts can fail to be.
    pub fn digest(&self) -> Result<Digest> {
        let mut hasher = Sha256::new();
        hasher.update(b"sapstudio-look-v1");
        hasher.update(&[match self.interpolation {
            Interpolation::Tetrahedral => 1,
            Interpolation::Trilinear => 2,
        }]);
        // The encoding goes in through a frame's own tagging, which already
        // has stable numbers for every colour field. A second tag table here
        // would be two statements of one fact, and the day they disagreed a
        // grade would change identity without changing.
        let format = if self.expects.matrix == MatrixCoefficients::Identity {
            PixelFormat::Rgb8
        } else {
            PixelFormat::Yuv444p8
        };
        let description = FrameDescription::square(
            Geometry::new(2, 2).map_err(RenderStatus::Media)?,
            format,
            self.expects,
            None,
            None,
        )
        .map_err(RenderStatus::Media)?;
        let witness = Frame::blank(description).map_err(RenderStatus::Media)?;
        hasher.update(witness.digest().bytes());

        let size = self.table.size();
        hasher.update(&u32::try_from(size).unwrap_or(u32::MAX).to_le_bytes());
        for blue in 0..size {
            for green in 0..size {
                for red in 0..size {
                    for component in self.table.sample(red, green, blue)? {
                        hasher.update(&component.raw().to_le_bytes());
                    }
                }
            }
        }
        Ok(hasher.finish())
    }

    /// Apply this look to a frame.
    ///
    /// The result is described exactly as the input was. A look changes what
    /// the numbers *are*, not what they mean: a table authored for a display
    /// encoding takes display-encoded values to display-encoded values, and
    /// claiming the output was in some other space would be a conversion
    /// nobody performed.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::LookSpaceMismatch`] if the frame is not in the encoding
    /// the table was authored for, [`RenderStatus::LookNotRgb`] for a format
    /// that is not red-green-blue, [`RenderStatus::LookPremultiplied`] for
    /// premultiplied coverage, and [`RenderStatus::OutOfMemory`].
    pub fn apply(&self, frame: &Frame) -> Result<Frame> {
        let description = *frame.description();
        if description.colour() != self.expects {
            return Err(RenderStatus::LookSpaceMismatch);
        }
        let channels = match description.format() {
            PixelFormat::Rgb8 => 3,
            PixelFormat::Rgba8 => 4,
            // A table maps three numbers to three. A luma-chroma frame needs
            // the matrix taken out of it first and a grey frame has nowhere to
            // put a colour, and each of those is a named step of its own
            // rather than something to do on the way past.
            _ => return Err(RenderStatus::LookNotRgb),
        };
        if description.alpha() == Some(AlphaState::Premultiplied) {
            return Err(RenderStatus::LookPremultiplied);
        }

        let range = description.colour().range;
        let packed = frame.to_packed().map_err(RenderStatus::Media)?;
        let mut out = Vec::new();
        out.try_reserve(packed.len())
            .map_err(|_| RenderStatus::OutOfMemory)?;

        for pixel in packed.chunks_exact(channels) {
            let mut colour = [crate::Fixed::ZERO; 3];
            for (channel, slot) in colour.iter_mut().enumerate() {
                *slot = normalise(pixel[channel], range, false)?;
            }
            let looked = self.table.look_up(colour, self.interpolation)?;
            for component in looked {
                out.push(quantise(component, range, false)?);
            }
            if channels == 4 {
                // Coverage carried through untouched. A table knows nothing
                // about alpha, and the last time something in this crate wrote
                // a constant here instead, every keyed frame that went through
                // it came out a solid rectangle.
                out.push(pixel[3]);
            }
        }

        Frame::from_packed(description, &out).map_err(RenderStatus::Media)
    }
}
