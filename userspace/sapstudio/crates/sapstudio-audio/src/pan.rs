// SPDX-License-Identifier: GPL-3.0-only
//! Panning, and the law that decides what "centre" means.
//!
//! A pan control sends one signal to two speakers, and the question it has to
//! answer is what to do at the middle. Send full level to both and the centre
//! is 3 dB *louder* than either side, because two speakers carrying the same
//! signal sum in pressure. Send half to each and the centre is 6 dB quieter
//! than the sides in power, so anything panned centre drops away.
//!
//! The answer used here is **constant power**: the two gains satisfy
//!
//! ```text
//! left² + right² = 1
//! ```
//!
//! at every position, so the total energy leaving the pair does not change as
//! a source moves across the image. Centre is therefore `√½` on both sides —
//! about −3.01 dB, not 0 dB and not −6 dB — which is the "3 dB pan law" every
//! console has a switch for.
//!
//! The usual way to compute it is `cos` and `sin` of a quarter turn, which
//! needs trigonometry. It does not have to: substituting `p` for the fraction
//! of the way across gives `√(1 − p)` and `√p` directly, which satisfies the
//! same identity exactly and needs only a square root — and
//! [`sapstudio_core::Fixed`] has an integer one. No libm, no floating point,
//! and the same answer on every machine (R-4.1).
//!
//! The image it draws is not identical to the trigonometric law — the two
//! agree at the three positions anybody checks and differ by at most a
//! fraction of a decibel between them — and this one is exact where the other
//! would need a table somebody has to trust.

use sapstudio_core::{Fixed, Rational};

use crate::status::{AudioStatus, Result};

/// Where a source sits between the speakers.
///
/// Minus one is hard left, zero is centre, plus one is hard right. Held as an
/// exact rational, because a pan moved and moved back must land where it
/// started.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Pan {
    position: Rational,
}

impl Pan {
    /// Dead centre.
    pub const CENTRE: Self = Self {
        position: Rational::ZERO,
    };

    /// A position between hard left and hard right.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::PanOutOfRange`] outside minus one to plus one.
    pub fn new(position: Rational) -> Result<Self> {
        let one = Rational::ONE;
        if position < one.checked_neg()? || position > one {
            return Err(AudioStatus::PanOutOfRange);
        }
        Ok(Self { position })
    }

    /// Hard left.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn left() -> Result<Self> {
        Self::new(Rational::ONE.checked_neg()?)
    }

    /// Hard right.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn right() -> Result<Self> {
        Self::new(Rational::ONE)
    }

    /// Where this sits.
    #[must_use]
    pub const fn position(self) -> Rational {
        self.position
    }

    /// The mirror image of this position.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn mirrored(self) -> Result<Self> {
        Self::new(self.position.checked_neg()?)
    }

    /// The gains this position sends to the left and right speakers.
    ///
    /// Constant power: the squares sum to one at every position, so moving a
    /// source across the image does not change how loud it is.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn gains(self) -> Result<(Fixed, Fixed)> {
        // The position runs from minus one to plus one; the fraction of the
        // way across runs from zero to one.
        let two = Rational::new(2, 1)?;
        let fraction = self.position.checked_add(Rational::ONE)?.checked_div(two)?;
        let right = Fixed::from_rational(fraction)?;
        let left = Fixed::ONE.checked_sub(right)?;
        Ok((left.sqrt()?, right.sqrt()?))
    }
}
