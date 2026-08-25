// SPDX-License-Identifier: GPL-3.0-only
//! What a track is made of.
//!
//! An item is a clip or a gap. Both have a length; only a clip has a source.
//! A gap is an item rather than a hole, because arithmetic over holes is where
//! timelines go wrong: with explicit gaps, a track's contents always tile its
//! whole length with no overlap and no missing region, by construction.

use sapstudio_core::{Duration, Instant, Timebase};

use crate::media::MediaId;
use crate::status::{ModelStatus, Result};

/// A reference to a range of a media asset, placed on a track.
///
/// The source position is counted in the track's timebase. For this milestone
/// a clip's media must share the sequence's timebase; mixed-rate cutting is a
/// later contract with its own conversion rules, not a widening of this one
/// (R-1.2).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Clip {
    media: MediaId,
    source_start: i64,
    duration: Duration,
    grade: Option<crate::media::Digest>,
    mask: Option<crate::mask::Mask>,
}

impl Clip {
    /// Refer to `duration` of `media`, beginning `source_start` ticks in.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, and
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn new(media: MediaId, source_start: i64, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            media,
            source_start,
            duration,
            grade: None,
            mask: None,
        })
    }

    /// The media this cuts from.
    #[must_use]
    pub const fn media(&self) -> MediaId {
        self.media
    }

    /// The first tick of the media this uses.
    #[must_use]
    pub const fn source_start(&self) -> i64 {
        self.source_start
    }

    /// How much of the media this uses.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        self.duration
    }

    /// The first tick of the media past this clip.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_end(&self) -> Result<i64> {
        self.source_start
            .checked_add(self.duration.ticks())
            .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))
    }

    /// The source position as an instant in the track's timebase.
    #[must_use]
    pub const fn source_instant(&self, timebase: Timebase) -> Instant {
        Instant::new(self.source_start, timebase)
    }

    /// The look applied to this clip, if it has one.
    ///
    /// Named by the digest of the table rather than by an index into anything,
    /// for exactly the reason media is: the same grade in two projects is the
    /// same grade, a project-local handle would cache it twice, and a file
    /// swapped underneath a handle is a different look wearing the same name.
    ///
    /// The model holds the digest and not the table. A cube is 35,937 triples
    /// and the model is structure — and `sapstudio-render`, where a table
    /// lives, sits above the model rather than beside it, so holding one here
    /// would invert the layering as well as the size.
    #[must_use]
    pub const fn grade(&self) -> Option<crate::media::Digest> {
        self.grade
    }

    /// The mask on this clip, if it has one.
    #[must_use]
    pub const fn mask(&self) -> Option<&crate::mask::Mask> {
        self.mask.as_ref()
    }

    /// The same clip with a mask, or with none.
    #[must_use]
    pub fn with_mask(&self, mask: Option<crate::mask::Mask>) -> Self {
        Self {
            mask,
            ..self.clone()
        }
    }

    /// The same clip with a look on it, or with none.
    #[must_use]
    pub fn with_grade(&self, grade: Option<crate::media::Digest>) -> Self {
        Self {
            grade,
            ..self.clone()
        }
    }

    /// The same clip reading from a different point of its media.
    ///
    /// This exists so that a slip is written as a change to *one* field rather
    /// than as a rebuild from three of them. A rebuild through
    /// [`Clip::new`] starts from nothing, which is right for a clip that is
    /// new and wrong for a clip that already existed — and when the grade was
    /// added, that difference silently dropped the look off every slipped
    /// clip. A test caught it at the third such site after two were found by
    /// reading, which is two more than should have needed finding.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn with_source(&self, source_start: i64) -> Result<Self> {
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            source_start,
            ..self.clone()
        })
    }

    /// The same clip cut to a different length, keeping everything else.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(Self {
            duration,
            ..self.clone()
        })
    }
}

/// One entry on a track.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Item {
    /// Material.
    Clip(Clip),
    /// Deliberate absence of material, with a length.
    Gap(Duration),
}

impl Item {
    /// A gap of a given length.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn gap(duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(Self::Gap(duration))
    }

    /// How long this item occupies its track.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        match self {
            Self::Clip(clip) => clip.duration(),
            Self::Gap(duration) => *duration,
        }
    }

    /// The timebase this item is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.duration().timebase()
    }

    /// This item with a different length, keeping its source position.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, or a timebase mismatch.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.timebase() != self.timebase() {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(match self {
            // The grade travels with the clip. A trim is a change of length,
            // and a clip that lost its look because somebody shortened it
            // would be the kind of fault nobody thinks to look for.
            Self::Clip(clip) => Self::Clip(clip.with_duration(duration)?),
            Self::Gap(_) => Self::Gap(duration),
        })
    }

    /// This item cut in two at `offset` ticks from its start.
    ///
    /// The two pieces are contiguous in their source, so joining them is the
    /// exact inverse of this operation.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SplitOutsideItem`] if the offset would leave either
    /// piece empty.
    pub fn split(&self, offset: i64) -> Result<(Self, Self)> {
        if offset <= 0 || offset >= self.duration().ticks() {
            return Err(ModelStatus::SplitOutsideItem);
        }
        let timebase = self.timebase();
        let head_length = Duration::new(offset, timebase).map_err(ModelStatus::from)?;
        let tail_length = self.duration().checked_sub(head_length)?;
        let head = self.with_duration(head_length)?;
        let tail = match self {
            Self::Clip(clip) => {
                let source_start = clip
                    .source_start
                    .checked_add(offset)
                    .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
                // The tail carries the grade too. `with_duration` gives the
                // head one and `Clip::new` gives the tail none, so writing
                // this the obvious way made a split lose the look off half of
                // what it cut — and left join no longer the exact inverse of
                // split, which is the property the two are built around.
                Self::Clip(clip.with_source(source_start)?.with_duration(tail_length)?)
            }
            Self::Gap(_) => Self::Gap(tail_length),
        };
        Ok((head, tail))
    }

    /// Whether `later` continues this item without a break in its source.
    #[must_use]
    pub fn continues_into(&self, later: &Self) -> bool {
        match (self, later) {
            (Self::Gap(_), Self::Gap(_)) => self.timebase() == later.timebase(),
            (Self::Clip(first), Self::Clip(second)) => {
                first.media == second.media
                    && first.duration.timebase() == second.duration.timebase()
                    // The same look, too. Two clips of one piece of media,
                    // adjacent in its source and graded differently, are not
                    // one item — they are two shots with two looks, and
                    // joining them would keep the first's and discard the
                    // second's without saying so. Refusing keeps join the
                    // exact inverse of split, which only cuts one look in two.
                    && first.grade == second.grade
                    && first
                        .source_end()
                        .is_ok_and(|end| end == second.source_start)
            }
            _ => false,
        }
    }

    /// This item and the one that continues it, as one item.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ItemsNotContiguous`] if the two are not adjacent in
    /// their source, which is what makes join the exact inverse of split.
    pub fn join(&self, later: &Self) -> Result<Self> {
        if !self.continues_into(later) {
            return Err(ModelStatus::ItemsNotContiguous);
        }
        let duration = self.duration().checked_add(later.duration())?;
        self.with_duration(duration)
    }
}
