// SPDX-License-Identifier: GPL-3.0-only
//! A shape that decides which part of a clip is used.
//!
//! A mask is to *where* what a grade is to *colour*: a property of the clip
//! rather than of the track, carried by the layer stack, and applied by the
//! renderer. It is the same primitive a wipe is made of — an exact-area
//! coverage plane — pointed at a clip instead of at a transition.
//!
//! ## In fractions of the frame, not in pixels
//!
//! Every corner is a fraction of the frame's width and height, so a mask drawn
//! on a proxy is the same mask on the finish, and a project conformed to
//! another size keeps the shape somebody drew rather than a pixel count that
//! no longer means anything. Nought is the left or top edge and one is the
//! right or bottom; a corner outside that is not refused, because a mask whose
//! points sit off the frame is how an editor says "all the way to the edge and
//! past it" rather than a mistake.
//!
//! ## Convex, and what that costs
//!
//! The corners must describe a convex polygon, because that is what the
//! rasteriser computes an exact area for: a convex region is an intersection
//! of half-planes and the pixel square can be clipped against each in turn.
//!
//! A concave mask is a **union of convex ones** rather than a different kind
//! of shape, and the union is not built here. That is a real limitation and it
//! is named rather than hidden: an editor who draws a concave outline gets a
//! refusal that says so, not a mask quietly repaired into its convex hull —
//! which would be a different shape, drawn by nobody, and impossible to notice
//! until something went to air.

use alloc::vec::Vec;

use sapstudio_core::Rational;

use crate::status::{ModelStatus, Result};

/// The most corners one mask may have.
///
/// Thirty-two: an ellipse drawn as a polygon is convincing at sixteen and
/// past arguing at thirty-two, and it is a bound a hostile project file
/// cannot talk its way past (R-11.2).
pub const MAX_CORNERS: usize = 32;

/// The fewest.
pub const MIN_CORNERS: usize = 3;

/// A convex region of the frame, in fractions of it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mask {
    corners: Vec<(Rational, Rational)>,
    inverted: bool,
}

impl Mask {
    /// A mask from its corners, in order around the shape.
    ///
    /// Either direction round is accepted — an editor drags points in whatever
    /// order the shape came out, and insisting on one winding would refuse
    /// half of the shapes somebody actually draws.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MaskTooSimple`] for fewer than [`MIN_CORNERS`] corners,
    /// which enclose no area; [`ModelStatus::CapacityExhausted`] past
    /// [`MAX_CORNERS`]; and [`ModelStatus::MaskNotConvex`] for corners that
    /// turn both ways, which is a shape this build cannot compute an exact
    /// area for.
    pub fn new(corners: Vec<(Rational, Rational)>) -> Result<Self> {
        if corners.len() < MIN_CORNERS {
            return Err(ModelStatus::MaskTooSimple);
        }
        if corners.len() > MAX_CORNERS {
            return Err(ModelStatus::CapacityExhausted);
        }
        check_convex(&corners)?;
        Ok(Self {
            corners,
            inverted: false,
        })
    }

    /// An axis-aligned rectangle, from its left, top, right and bottom.
    ///
    /// # Errors
    ///
    /// As [`Mask::new`], and [`ModelStatus::MaskTooSimple`] for a rectangle
    /// enclosing nothing.
    pub fn rectangle(
        left: Rational,
        top: Rational,
        right: Rational,
        bottom: Rational,
    ) -> Result<Self> {
        if !right.checked_sub(left)?.is_positive() || !bottom.checked_sub(top)?.is_positive() {
            return Err(ModelStatus::MaskTooSimple);
        }
        Self::new(alloc::vec![
            (left, top),
            (right, top),
            (right, bottom),
            (left, bottom),
        ])
    }

    /// The corners, in the order they were given.
    #[must_use]
    pub fn corners(&self) -> &[(Rational, Rational)] {
        &self.corners
    }

    /// Whether what is kept is what lies *outside* the shape.
    ///
    /// A separate flag rather than a second kind of mask, because inverting is
    /// something an editor does to a shape they have already drawn and expects
    /// to be able to undo without redrawing it.
    #[must_use]
    pub const fn is_inverted(&self) -> bool {
        self.inverted
    }

    /// The same mask, keeping the other side.
    #[must_use]
    pub fn inverted(&self) -> Self {
        Self {
            corners: self.corners.clone(),
            inverted: !self.inverted,
        }
    }

    /// The same mask with its inversion set.
    #[must_use]
    pub fn with_inversion(&self, inverted: bool) -> Self {
        Self {
            corners: self.corners.clone(),
            inverted,
        }
    }
}

/// Whether every turn goes the same way.
///
/// The cross product of consecutive edges is positive for one direction of
/// turn and negative for the other, so a shape that produces both turns both
/// ways and is not convex. A zero is a corner that does not turn at all —
/// three points in a line — which is permitted: it describes the same region
/// as the shape without it, and refusing it would refuse a rectangle somebody
/// built by dragging a fifth point onto an edge.
fn check_convex(corners: &[(Rational, Rational)]) -> Result<()> {
    let mut sign = 0_i8;
    for index in 0..corners.len() {
        let a = corners[index];
        let b = corners[(index + 1) % corners.len()];
        let c = corners[(index + 2) % corners.len()];
        let turn =
            b.0.checked_sub(a.0)?
                .checked_mul(c.1.checked_sub(b.1)?)?
                .checked_sub(b.1.checked_sub(a.1)?.checked_mul(c.0.checked_sub(b.0)?)?)?;
        if turn.is_zero() {
            continue;
        }
        let here = if turn.is_positive() { 1 } else { -1 };
        if sign == 0 {
            sign = here;
        } else if sign != here {
            return Err(ModelStatus::MaskNotConvex);
        }
    }
    if sign == 0 {
        // Every corner in a line: a polygon with no area at all.
        return Err(ModelStatus::MaskTooSimple);
    }
    Ok(())
}
