// SPDX-License-Identifier: GPL-3.0-only
//! What a sequence shows at one instant.
//!
//! This is the question the whole timeline exists to answer, and it is a
//! question about the *model* rather than about pictures: given an instant,
//! which piece of which media is on each track, and in what order do they go
//! on top of one another. Turning those into a frame is somebody else's job.
//!
//! Three decisions are made here, and each is the kind that is normally left
//! implicit until it is a bug report.
//!
//! **Higher tracks are on top.** V1 is the bottom of the stack and V2 covers
//! it, which is what every editing application in this industry does and what
//! every editor expects. The list comes back bottom first, in the order it is
//! to be composited, so the order is data rather than a convention a caller
//! has to remember.
//!
//! **A gap is transparent, not black.** A gap on V2 shows V1 through it. This
//! is the difference between an insert edit and a hole punched in the
//! programme, and getting it wrong makes an upper track with sparse material
//! blank out everything beneath it — a bug that looks like the compositor's
//! fault and is not.
//!
//! **A track that does not reach contributes nothing**, in the same way a gap
//! does. A short track is not a track full of black past its end; it is a
//! track that has stopped.
//!
//! What is *not* decided here is what happens when the stack is empty. A
//! sequence with nothing at an instant shows nothing, and whether "nothing" is
//! black or transparent is a rendering policy — so this returns an empty list
//! and lets the caller say.

use alloc::vec::Vec;

use sapstudio_core::{Instant, Rational};

use crate::bounded::push_bounded;
use crate::media::MediaId;
use crate::sequence::Sequence;
use crate::status::{ModelStatus, Result};
use crate::track::{Track, TrackKind, Transition, TransitionKind, Wipe};

/// One piece of material, at one instant, on one track.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layer {
    track: usize,
    media: MediaId,
    source: i64,
    opacity: Rational,
    grade: Option<crate::media::Digest>,
    mask: Option<crate::mask::Mask>,
    wipe: Option<Revealed>,
}

/// The incoming side of a wipe, and how far its edge has travelled.
///
/// Only the incoming clip of a wipe carries one. The outgoing clip is whole
/// underneath it, exactly as it is under a dissolve — which is what lets a
/// wipe share the dissolve's whole structure and differ only in what the
/// renderer does with the number.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Revealed {
    wipe: Wipe,
    fraction: Rational,
}

impl Revealed {
    /// Which way the edge sweeps.
    #[must_use]
    pub const fn wipe(self) -> Wipe {
        self.wipe
    }

    /// How far along its travel the edge is, from none to all.
    ///
    /// The same exact fraction a dissolve would have faded to at this instant.
    /// A wipe and a dissolve of the same length are the same transition timed
    /// the same way; only what the fraction *means* differs, and that is the
    /// renderer's business rather than the model's.
    #[must_use]
    pub const fn fraction(self) -> Rational {
        self.fraction
    }
}

impl Layer {
    /// Which track this came from.
    #[must_use]
    pub const fn track(&self) -> usize {
        self.track
    }

    /// Which media to read.
    #[must_use]
    pub const fn media(&self) -> MediaId {
        self.media
    }

    /// The look on the clip this came from, if it has one.
    ///
    /// Carried up from the clip rather than looked up here, because the stack
    /// answers what is on each track and a table is not on a track — it is
    /// something a renderer will have to fetch, by the same digest, from the
    /// same place it fetches frames.
    #[must_use]
    pub const fn grade(&self) -> Option<crate::media::Digest> {
        self.grade
    }

    /// How much of this layer is showing, from none to all of it.
    ///
    /// One everywhere except inside a dissolve, where the outgoing clip fades
    /// from one to nothing and the incoming one rises to meet it. It is an
    /// exact rational — `n` frames into an `N`-frame dissolve is `n/N`, not a
    /// decimal near it — so a dissolve is the same dissolve on every machine
    /// and the two halves always sum to exactly one.
    #[must_use]
    pub const fn opacity(&self) -> Rational {
        self.opacity
    }

    /// The mask on the clip this came from, if it has one.
    ///
    /// Carried up rather than applied here for the same reason a grade is:
    /// the stack answers what is on each track, and a shape is not a fact
    /// about the track — it is something a renderer turns into coverage.
    #[must_use]
    pub const fn mask(&self) -> Option<&crate::mask::Mask> {
        self.mask.as_ref()
    }

    /// The wipe revealing this layer, if it is the incoming side of one.
    ///
    /// `None` for everything else, including the outgoing side of a wipe and
    /// both sides of a dissolve — a dissolve says everything it needs to
    /// through [`Layer::opacity`].
    #[must_use]
    pub const fn wipe(&self) -> Option<Revealed> {
        self.wipe
    }

    /// Which tick of that media, counted from its own start.
    ///
    /// This is the clip's source start plus how far into the clip the instant
    /// falls — the arithmetic that decides *which frame* a playhead shows, and
    /// the one an off-by-one makes wrong for the whole clip rather than for
    /// one frame of it.
    #[must_use]
    pub const fn source(&self) -> i64 {
        self.source
    }
}

/// Which track this stack is of.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Lane {
    /// The picture tracks.
    Picture,
    /// The sound tracks.
    Sound,
}

impl Lane {
    /// The kind of track this lane holds.
    const fn kind(self) -> TrackKind {
        match self {
            Self::Picture => TrackKind::Video,
            Self::Sound => TrackKind::Audio,
        }
    }
}

impl Sequence {
    /// What is on each track of one lane at an instant, bottom first.
    ///
    /// Tracks with a gap there, and tracks that do not reach that far,
    /// contribute nothing — so an empty result means the sequence shows
    /// nothing at that instant, which is a real answer.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch if the instant is
    /// not counted in the sequence's own timebase, [`ModelStatus::UnknownItem`]
    /// if a track's contents disagree with its own index, or
    /// [`ModelStatus::OutOfMemory`].
    pub fn stack_at(&self, lane: Lane, instant: Instant) -> Result<Vec<Layer>> {
        if instant.timebase() != self.timebase() {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        let mut stack = Vec::new();
        for (index, track) in self.tracks().iter().enumerate() {
            if track.kind() != lane.kind() {
                continue;
            }
            // A track's own opacity multiplies whatever the items on it are
            // doing. It is read once for the track rather than once per layer,
            // because a dissolve's two layers are on the same track at the
            // same instant and reading it twice would be asking one question
            // twice and hoping for the same answer.
            let animated = track.opacity_at(instant)?;
            if let Some(transition) = dissolve_at(track, instant)? {
                // Both sides of the cut are on screen. The outgoing one is at
                // full opacity underneath and the incoming one fades up over
                // it, which is a cross-fade written as an `over` — the mix is
                // `in x t + out x (1 - t)` either way, and this way the
                // compositor needs no second operator.
                //
                // A wipe is the same structure with the fraction meaning
                // something else: the incoming clip is *whole* rather than
                // faded, and the fraction says how far its edge has swept. So
                // both sides stay at full opacity and the fraction travels
                // with the layer instead of being spent on it.
                let travelled = fraction(track, transition, instant)?;
                let sweeping = match transition.kind() {
                    TransitionKind::Dissolve => None,
                    TransitionKind::Wipe(wipe) => Some(Revealed {
                        wipe,
                        fraction: travelled,
                    }),
                };
                let rising = if sweeping.is_some() {
                    Rational::ONE
                } else {
                    travelled
                };
                for (item, opacity, revealed) in [
                    (transition.boundary() - 1, Rational::ONE, None),
                    (transition.boundary(), rising, sweeping),
                ] {
                    let crate::Item::Clip(clip) = track.item(item)? else {
                        return Err(ModelStatus::NotAClip);
                    };
                    // The offset is measured from the item's own start, so it
                    // runs past the outgoing clip's end and before the
                    // incoming clip's beginning — which is exactly what
                    // handles are.
                    let offset = instant
                        .ticks()
                        .checked_sub(track.item_start(item)?.ticks())
                        .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
                    let source = clip
                        .source_start()
                        .checked_add(offset)
                        .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
                    push_bounded(
                        &mut stack,
                        Layer {
                            track: index,
                            media: clip.media(),
                            source,
                            opacity: opacity.checked_mul(animated)?,
                            grade: clip.grade(),
                            mask: clip.mask().cloned(),
                            wipe: revealed,
                        },
                        MAX_LAYERS,
                    )?;
                }
                continue;
            }
            let Some((item, offset)) = track.item_at(instant)? else {
                // The track has stopped. Not black past its end — stopped.
                continue;
            };
            let crate::Item::Clip(clip) = track.item(item)? else {
                // A gap is transparent, so whatever is beneath shows through.
                continue;
            };
            let source = clip
                .source_start()
                .checked_add(offset)
                .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
            push_bounded(
                &mut stack,
                Layer {
                    track: index,
                    media: clip.media(),
                    source,
                    opacity: animated,
                    grade: clip.grade(),
                    mask: clip.mask().cloned(),
                    wipe: None,
                },
                MAX_LAYERS,
            )?;
        }
        Ok(stack)
    }
}

/// Where a dissolve begins, relative to the cut it sits on.
///
/// Half its length, rounded down — so an even dissolve is centred exactly and
/// an odd one puts its extra frame after the cut. That is a choice rather than
/// a fact, and it is written here so that it is one place rather than several.
fn opening(transition: &Transition) -> i64 {
    transition.duration().ticks() / 2
}

/// The dissolve covering an instant on a track, if there is one.
fn dissolve_at(track: &Track, instant: Instant) -> Result<Option<Transition>> {
    for transition in track.transitions() {
        let cut = track.item_start(transition.boundary())?;
        let start = cut
            .ticks()
            .checked_sub(opening(transition))
            .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
        let end = start
            .checked_add(transition.duration().ticks())
            .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
        if instant.ticks() >= start && instant.ticks() < end {
            return Ok(Some(*transition));
        }
    }
    Ok(None)
}

/// How much of the incoming clip is showing, as an exact fraction.
///
/// An `N`-frame dissolve runs from `1/(N+1)` to `N/(N+1)`, never touching
/// nought or one. That is deliberate: a dissolve whose first frame is entirely
/// the outgoing clip has wasted a frame showing what the frame before it
/// already showed, and the same at the other end. Every frame of a dissolve is
/// a real mix.
fn fraction(track: &Track, transition: Transition, instant: Instant) -> Result<Rational> {
    let cut = track.item_start(transition.boundary())?;
    let start = cut
        .ticks()
        .checked_sub(opening(&transition))
        .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
    let elapsed = instant
        .ticks()
        .checked_sub(start)
        .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
    let length = transition.duration().ticks();
    Ok(Rational::new(elapsed + 1, length + 1)?)
}

/// The most layers one instant may stack.
///
/// Twice a sequence's track bound, because a track inside a dissolve names
/// *two* layers — both sides of the cut are on screen at once. Bounded by
/// something the caller already agreed to (R-11.2).
const MAX_LAYERS: usize = 2 * crate::sequence::MAX_TRACKS_PER_SEQUENCE;
