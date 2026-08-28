// SPDX-License-Identifier: GPL-3.0-only
//! Where a clip sits in the frame, and how big it is.
//!
//! A transform is to *geometry* what a grade is to colour and a mask is to
//! extent: a property of the clip, carried by the layer stack, applied by the
//! renderer.
//!
//! ## Dimensionless, and about the centre
//!
//! The linear part is dimensionless — twice the size is twice the size at
//! every resolution — and the translation is in **fractions of the frame**, so
//! a move of a tenth is a tenth of the way across whatever the picture is
//! delivered at. A project cut on a proxy and finished at four times the size
//! keeps the framing somebody chose rather than keeping a pixel count that no
//! longer means the same thing.
//!
//! And it acts about the frame's **centre**. Scaling about the corner is what
//! the arithmetic does if nobody decides otherwise, and it sends the picture
//! sliding off to the lower right the moment somebody drags a scale slider —
//! which is not what anybody means by "make it bigger".
//!
//! ## An angle is not a matrix
//!
//! There is no rotation-in-degrees here, for the reason the wipe's direction
//! is a vector rather than an angle: a sine and a cosine are not exact, and a
//! project whose framing depended on them would drift. The linear part is four
//! rationals. An interface that offers a dial converts once, when somebody
//! turns it, and the approximation lives at the edge of the system instead of
//! in every frame of every render.
//!
//! The pleasant consequence is that the transforms people actually use are
//! exact: a half, a third, a mirror, a quarter turn.
//!
//! ## But a turn can be exact, and this is how
//!
//! The paragraph above is right that *degrees* are not exact and wrong to stop
//! there, and [`Turn`] is the difference. A rotation is a point on the unit
//! circle, and the rational points on the unit circle are not scarce — they are
//! **dense**, and there is a formula for every one of them. Put `t = tan(θ/2)`
//! and
//!
//! ```text
//!     cos θ = (1 − t²) / (1 + t²)        sin θ = 2t / (1 + t²)
//! ```
//!
//! which is a rational whenever `t` is, with `cos² + sin² = 1` exactly and a
//! determinant of exactly one. `t = 1` is a quarter turn, `t = 1/3` is the
//! three-four-five triangle at about 36.87°, and every rational between them
//! is an exact rotation nobody had to approximate.
//!
//! So a turn is stored as the point rather than as the parameter — `(−1, 0)`
//! is a half turn and no finite `t` reaches it — and the parameter is how one
//! is *built*, and how one is **animated**, because `t` runs over the whole
//! line while the angle runs over an interval and a curve needs somewhere
//! unbounded to live.

use alloc::vec::Vec;

use sapstudio_core::{Instant, Rational};

use crate::curve::Curve;
use crate::status::{ModelStatus, Result};

/// How to weigh the source under a destination pixel.
///
/// Named here rather than imported, because the renderer is this crate's
/// *sibling* and cannot be depended on. The model owns the decision — which
/// filter somebody chose is part of their project — and the renderer owns what
/// the decision means.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Resampling {
    /// The exact area-weighted mean of the source a destination pixel covers.
    /// Right for reduction.
    Area,
    /// The four samples around where a destination pixel's centre lands.
    /// Right for enlargement.
    Bilinear,
}

/// A rotation, held as an exact point on the unit circle.
///
/// Not an angle, and not a matrix either. An angle in degrees needs a sine and
/// a cosine to become geometry, and neither is exact; a general matrix can
/// shear, and a shear is a different feature. A point `(cos θ, sin θ)` with
/// `cos² + sin² = 1` is exactly a rotation and exactly nothing else, and the
/// rational ones are dense — see this module's header for the parametrisation
/// that reaches every one of them.
///
/// The determinant is one by construction, so a turn preserves area, preserves
/// convexity, and preserves winding. That is why [`Mask::turned_by`] can hand
/// back a mask rather than something that might refuse mid-render, and it is
/// the same argument [`Mask::moved_by`] makes about a positive scale.
///
/// [`Mask::turned_by`]: crate::mask::Mask::turned_by
/// [`Mask::moved_by`]: crate::mask::Mask::moved_by
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Turn {
    cosine: Rational,
    sine: Rational,
}

impl Turn {
    /// No rotation at all.
    pub const NONE: Self = Self {
        cosine: Rational::ONE,
        sine: Rational::ZERO,
    };

    /// A turn from a point on the unit circle.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NotATurn`] if the point is not *on* the circle. Anything
    /// else is a scale wearing a rotation's name — and a caller who wanted a
    /// scale has [`Motion`]'s own lane for it, where it is checked for being
    /// positive rather than being smuggled in through a matrix.
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn new(cosine: Rational, sine: Rational) -> Result<Self> {
        if cosine
            .checked_mul(cosine)?
            .checked_add(sine.checked_mul(sine)?)?
            != Rational::ONE
        {
            return Err(ModelStatus::NotATurn);
        }
        Ok(Self { cosine, sine })
    }

    /// The turn whose half-angle has tangent `parameter`.
    ///
    /// The map `t ↦ 2·arctan(t)`, computed without a trigonometric function
    /// anywhere: `cos = (1 − t²)/(1 + t²)` and `sin = 2t/(1 + t²)`, both exact.
    ///
    /// Nought is no turn, one is a quarter turn, and the whole line maps onto
    /// the open interval between a half turn each way. That last clause is the
    /// reason this is a constructor rather than the representation: a half
    /// turn is `(−1, 0)` and no finite parameter reaches it, so a type that
    /// stored `t` could not express turning something upside down.
    ///
    /// It is also **not** constant angular speed. `dθ/dt = 2/(1 + t²)`, so a
    /// straight ramp in `t` sweeps fastest through nought and slows as it
    /// approaches a half turn either way. Constant angular speed is not
    /// available in exact arithmetic at all: it would need the angle itself
    /// interpolated, which needs the two functions this type exists to avoid.
    /// Composing a fixed small turn once per frame *would* be exact and is
    /// unrepresentable for a different reason — the denominators multiply, so
    /// a fifth of a right angle repeated fifty times has a denominator past
    /// what an `i64` holds long before the shot is over.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn from_half_angle(parameter: Rational) -> Result<Self> {
        let square = parameter.checked_mul(parameter)?;
        let denominator = Rational::ONE.checked_add(square)?;
        // Never nought: `1 + t²` is at least one for every rational `t`.
        Ok(Self {
            cosine: Rational::ONE
                .checked_sub(square)?
                .checked_div(denominator)?,
            sine: parameter
                .checked_mul(Rational::new(2, 1)?)?
                .checked_div(denominator)?,
        })
    }

    /// The cosine of the angle turned through.
    #[must_use]
    pub const fn cosine(self) -> Rational {
        self.cosine
    }

    /// And its sine.
    #[must_use]
    pub const fn sine(self) -> Rational {
        self.sine
    }

    /// Whether this turns nothing.
    ///
    /// Worth asking for the reason [`Transform::is_still`] is: a shape nobody
    /// turned must come back the shape it was, corner for corner, rather than
    /// through a rotation that happens to be the identity.
    #[must_use]
    pub fn is_still(self) -> bool {
        self.cosine == Rational::ONE && self.sine.is_zero()
    }

    /// This turn followed by another, exactly.
    ///
    /// The angle-addition formulae, which stay on the circle: if both operands
    /// satisfy `c² + s² = 1` then so does the product, by the Brahmagupta
    /// identity, with no renormalisation and therefore no drift. A turn applied
    /// a thousand times is a turn through a thousand times the angle and is
    /// still exactly a turn.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow — which is the real bound
    /// here, because the denominators multiply.
    pub fn composed_with(self, other: Self) -> Result<Self> {
        Ok(Self {
            cosine: self
                .cosine
                .checked_mul(other.cosine)?
                .checked_sub(self.sine.checked_mul(other.sine)?)?,
            sine: self
                .sine
                .checked_mul(other.cosine)?
                .checked_add(self.cosine.checked_mul(other.sine)?)?,
        })
    }

    /// This point, turned.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub(crate) fn applied_to(self, (x, y): (Rational, Rational)) -> Result<(Rational, Rational)> {
        Ok((
            x.checked_mul(self.cosine)?
                .checked_sub(y.checked_mul(self.sine)?)?,
            x.checked_mul(self.sine)?
                .checked_add(y.checked_mul(self.cosine)?)?,
        ))
    }
}

/// Where a clip sits in the frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Transform {
    linear: [Rational; 4],
    offset: (Rational, Rational),
    resampling: Resampling,
    /// The point the linear part acts about, in fractions of the frame.
    ///
    /// The centre until somebody moves it, which is what every constructor
    /// here starts from. A pivot is not required to be *inside* the frame:
    /// swinging a card in from off the top of the picture is an anchor above
    /// nought, and refusing it would refuse a move somebody actually makes.
    anchor: (Rational, Rational),
}

/// The middle of the frame, which is where a transform acts about until
/// somebody says otherwise.
const CENTRE: (Rational, Rational) = (Rational::HALF, Rational::HALF);

impl Transform {
    /// A scale about the frame's centre, and a move in fractions of it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::TransformNotInvertible`] for a scale of nought on either
    /// axis, which flattens the picture onto a line and has no way back.
    pub fn scaled(
        across: Rational,
        down: Rational,
        offset: (Rational, Rational),
        resampling: Resampling,
    ) -> Result<Self> {
        Self::new(
            [across, Rational::ZERO, Rational::ZERO, down],
            offset,
            resampling,
        )
    }

    /// A transform from its linear part, row-major, and its move.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::TransformNotInvertible`] if the linear part has no
    /// inverse, and [`ModelStatus::Time`] wrapping an overflow.
    pub fn new(
        linear: [Rational; 4],
        offset: (Rational, Rational),
        resampling: Resampling,
    ) -> Result<Self> {
        let [a, b, c, d] = linear;
        let determinant = a.checked_mul(d)?.checked_sub(b.checked_mul(c)?)?;
        if determinant.is_zero() {
            return Err(ModelStatus::TransformNotInvertible);
        }
        Ok(Self {
            linear,
            offset,
            resampling,
            anchor: CENTRE,
        })
    }

    /// The point this acts about, in fractions of the frame.
    #[must_use]
    pub const fn anchor(&self) -> (Rational, Rational) {
        self.anchor
    }

    /// The same transform pivoting somewhere else.
    ///
    /// A `with_` rather than a fifth argument to two constructors, and that is
    /// this project's own rule rather than taste: a constructor starts from
    /// nothing and everything else changes one field, so a field added
    /// tomorrow travels through every builder without anybody remembering.
    ///
    /// There is no refusal. Every point is a pivot, including points outside
    /// the frame — a card swinging in from above the picture turns about a
    /// point above it — so a bound here would be a bound on moves rather than
    /// on values.
    #[must_use]
    pub fn with_anchor(&self, anchor: (Rational, Rational)) -> Self {
        Self { anchor, ..*self }
    }

    /// The linear part, row-major and dimensionless.
    #[must_use]
    pub const fn linear(&self) -> [Rational; 4] {
        self.linear
    }

    /// The move, in fractions of the frame.
    #[must_use]
    pub const fn offset(&self) -> (Rational, Rational) {
        self.offset
    }

    /// Which filter this asks for.
    #[must_use]
    pub const fn resampling(&self) -> Resampling {
        self.resampling
    }

    /// Whether this moves nothing.
    ///
    /// Worth asking, because a clip nobody has transformed must render through
    /// no resampler at all rather than through one that happens to be the
    /// identity: exact is a stronger promise than "the arithmetic works out",
    /// and it is the promise a project deserves after being opened and saved
    /// a hundred times.
    ///
    /// The anchor is deliberately **not** consulted. The identity fixes every
    /// point, so it fixes the anchor too, and a transform that moves nothing
    /// moves nothing whichever point it was going to move nothing about.
    /// Including the anchor here would send every clip with a pivot through a
    /// resampler to compute the picture it already had.
    #[must_use]
    pub fn is_still(&self) -> bool {
        self.linear == [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE]
            && self.offset.0.is_zero()
            && self.offset.1.is_zero()
    }
}

/// A transform that changes over the length of its clip.
///
/// M4.6 opened by naming three things a curve is for: "opacity that fades, a
/// **scale that pushes in**, a volume that ducks under dialogue." Two of them
/// got lanes and the third did not, and its State line said why — curves on
/// items "need a name for a keyframe that survives its item being renumbered".
///
/// That worry dissolves by putting the curve **on the clip**. There is no
/// index to survive: the animation is a field of the thing it animates, so it
/// travels with the clip when the clip moves, and a ripple that renumbers
/// every item after it renumbers nothing here.
///
/// ## Measured from the clip's own start
///
/// Which is the other half of the same decision. A keyframe at tick 12 is
/// twelve ticks into *this clip*, not twelve ticks into the programme — so
/// sliding a clip down the timeline slides its push-in with it, and trimming
/// its head does not silently re-time the move. A curve measured from the
/// timeline would make "move this shot later" and "re-animate this shot" the
/// same gesture.
///
/// ## What it animates, and what it does not
///
/// A uniform scale, a move, and a **turn**. Not the whole linear part: a matrix
/// that changes over time is four curves that can disagree about whether the
/// picture is still a rectangle, and the shear that produces is a different
/// feature. These three cannot disagree about that — a positive scale, a
/// rotation and a translation compose to a **similarity**, which takes a
/// rectangle to a rectangle and a convex outline to a convex outline for every
/// value the curves can take. That is the same argument the scale made on its
/// own, and it is why the turn was allowed to join it rather than waiting for
/// a general matrix.
///
/// The base transform still holds the shape — a mirror, a fixed turn — and
/// this scales, turns and moves it.
///
/// The turn's lane carries the **half-angle parameter** rather than an angle
/// or a point, because a curve needs somewhere unbounded to live: `t` runs
/// over the whole line, and [`Turn::from_half_angle`] takes it to the open
/// interval between a half turn each way. A lane holding a cosine and a sine
/// would be two curves that can leave the circle, which is the same defect as
/// four curves that can shear.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Motion {
    scale: Option<Curve>,
    across: Option<Curve>,
    down: Option<Curve>,
    turn: Option<Curve>,
}

impl Motion {
    /// A motion from its lanes, at least one of which must be present.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoAutomation`] if every lane is absent — a motion that
    /// animates nothing is a transform, and saying so is better than holding
    /// an empty one that reads as animated.
    /// [`ModelStatus::ScaleNotPositive`] for a scale keyframe at nought or
    /// below: a scale of nought flattens the picture onto a point, and a
    /// negative one is a mirror, which belongs in the base transform rather
    /// than in a move that would pass through nothing on its way there.
    pub fn new(
        scale: Option<Curve>,
        across: Option<Curve>,
        down: Option<Curve>,
        turn: Option<Curve>,
    ) -> Result<Self> {
        if scale.is_none() && across.is_none() && down.is_none() && turn.is_none() {
            return Err(ModelStatus::NoAutomation);
        }
        if let Some(held) = &scale {
            for keyframe in held.keyframes() {
                if !keyframe.value().is_positive() {
                    return Err(ModelStatus::ScaleNotPositive);
                }
            }
        }
        Ok(Self {
            scale,
            across,
            down,
            turn,
        })
    }

    /// The scale lane, if there is one.
    #[must_use]
    pub const fn scale(&self) -> Option<&Curve> {
        self.scale.as_ref()
    }

    /// The lane moving the picture across.
    #[must_use]
    pub const fn across(&self) -> Option<&Curve> {
        self.across.as_ref()
    }

    /// The lane moving it down.
    #[must_use]
    pub const fn down(&self) -> Option<&Curve> {
        self.down.as_ref()
    }

    /// The lane turning it, in half-angle parameter.
    ///
    /// Nought is no turn and one is a quarter turn. Not degrees, and not a
    /// cosine: see [`Turn::from_half_angle`] for why the parameter is what a
    /// curve is allowed to hold.
    #[must_use]
    pub const fn turn(&self) -> Option<&Curve> {
        self.turn.as_ref()
    }

    /// The same motion measured from a start `by` ticks further along.
    ///
    /// A split hands the tail a new start, and the tail's animation must be
    /// re-based onto it or the move restarts at the cut.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn shifted(&self, by: i64) -> Result<Self> {
        let shift = |lane: Option<&Curve>| -> Result<Option<Curve>> {
            match lane {
                None => Ok(None),
                Some(curve) => Ok(Some(curve.shifted(by)?)),
            }
        };
        Ok(Self {
            scale: shift(self.scale.as_ref())?,
            across: shift(self.across.as_ref())?,
            down: shift(self.down.as_ref())?,
            turn: shift(self.turn.as_ref())?,
        })
    }

    /// What the four lanes read at an instant measured from the clip's start.
    ///
    /// A lane with no curve reads its neutral value: one for the scale, which
    /// multiplies, nought for the moves, which add, and nought for the turn's
    /// parameter, which is [`Turn::NONE`].
    ///
    /// The turn comes back **resolved**, as a point on the circle rather than
    /// as the parameter it was stored as. That is the same decision the layer
    /// stack makes about everything else it hands downwards: a consumer should
    /// receive geometry, not a recipe for it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch, or whatever the
    /// curve refuses.
    pub fn at(&self, into: Instant) -> Result<(Rational, Rational, Rational, Turn)> {
        let read = |lane: Option<&Curve>, neutral: Rational| -> Result<Rational> {
            match lane {
                None => Ok(neutral),
                Some(curve) => curve.value_at(into),
            }
        };
        Ok((
            read(self.scale.as_ref(), Rational::ONE)?,
            read(self.across.as_ref(), Rational::ZERO)?,
            read(self.down.as_ref(), Rational::ZERO)?,
            Turn::from_half_angle(read(self.turn.as_ref(), Rational::ZERO)?)?,
        ))
    }
}

impl Transform {
    /// This transform scaled, turned and moved by what a motion read.
    ///
    /// The scale multiplies the linear part, the turn is applied **on the
    /// left** of it, and the moves add to the offset — which is what makes a
    /// base transform the *framing* and a motion the change to it: a mirror
    /// stays a mirror while it pushes in.
    ///
    /// On the left rather than on the right, and that is a decision rather
    /// than an arrangement. `R·M` turns the picture as the viewer sees it;
    /// `M·R` turns the source before the framing is applied, so a mirrored
    /// clip would appear to turn the other way. Rotation and reflection do not
    /// commute, which is exactly why the side has to be chosen and said. The
    /// *scale* is a scalar and commutes with both, which is why it never
    /// needed a sentence.
    ///
    /// The turn needs no refusal of its own. Its determinant is one, so the
    /// product's determinant is the scale's contribution alone, and a map that
    /// was invertible before the turn is invertible after it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ScaleNotPositive`] if the scale reads nought or below —
    /// which keyframes cannot be, but an *ease* between two positive ones can
    /// overshoot to, because an ease's verticals are deliberately unclamped.
    /// That is a guard reached only by an input that reaches it, so there is a
    /// test whose ease dips below nothing on the way between two scales.
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn moved_by(
        &self,
        scale: Rational,
        across: Rational,
        down: Rational,
        turn: Turn,
    ) -> Result<Self> {
        if !scale.is_positive() {
            return Err(ModelStatus::ScaleNotPositive);
        }
        let mut linear = self.linear;
        for held in &mut linear {
            *held = held.checked_mul(scale)?;
        }
        // `R·M`, written out: the turn acts on each column of the scaled
        // linear part, which is the same as acting on the two vectors the
        // matrix sends the axes to.
        let (a, c) = turn.applied_to((linear[0], linear[2]))?;
        let (b, d) = turn.applied_to((linear[1], linear[3]))?;
        Ok(Self::new(
            [a, b, c, d],
            (
                self.offset.0.checked_add(across)?,
                self.offset.1.checked_add(down)?,
            ),
            self.resampling,
        )?
        // `new` starts from nothing, which is right there and wrong here: an
        // animated clip pivots where its base transform pivots, and rebuilding
        // through the constructor would quietly move every animated clip's
        // pivot back to the centre. This is the third field to find that trap,
        // after the grade and the motion.
        .with_anchor(self.anchor))
    }
}

/// The four lanes of a motion, in the order the format writes them.
///
/// The turn is written last, after the three that were there before it, so
/// that the file's lane order and this function's order stay the same list —
/// which is what a reader comparing the two halves of the format checks.
#[must_use]
pub fn lanes(motion: &Motion) -> Vec<Option<&Curve>> {
    alloc::vec![
        motion.scale(),
        motion.across(),
        motion.down(),
        motion.turn()
    ]
}
