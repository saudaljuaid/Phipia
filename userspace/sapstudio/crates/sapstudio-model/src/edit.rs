// SPDX-License-Identifier: GPL-3.0-only
//! Edits, and the exact inverse of each one.
//!
//! An edit is a value: typed, comparable, and invertible. Applying one returns
//! the edit that undoes it, so history is a list of pairs and undo is not a
//! special mode — it is applying the other half of a pair (R-9.2).
//!
//! Every variant here is total in the sense that matters: it either performs
//! its whole change and returns its exact inverse, or it changes nothing and
//! returns a named refusal. There is no half-applied edit (R-1.4).

use sapstudio_core::Duration;

use crate::item::Item;
use crate::sequence::Sequence;
use crate::status::{ModelStatus, Result};
use crate::track::TrackKind;

/// One change to a sequence.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Edit {
    /// Place an item, moving everything after it later.
    InsertItem {
        /// Which track.
        track: usize,
        /// Where in the track's item list.
        index: usize,
        /// What to place.
        item: Item,
    },
    /// Take an item out, moving everything after it earlier.
    RemoveItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
    },
    /// Change an item's length, moving everything after it. This is a ripple
    /// trim of the item's tail.
    SetItemDuration {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The new length.
        duration: Duration,
    },
    /// Change which part of its media a clip uses, without moving it or
    /// changing its length. This is a slip.
    SetClipSource {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The new first tick of the media.
        source_start: i64,
    },
    /// Put a look on a clip, or take one off.
    ///
    /// The look is named by the digest of its table, for the same reason media
    /// is: the same grade in two projects is the same grade, and a
    /// project-local handle would cache it twice and could not tell that the
    /// file behind it had been swapped.
    SetClipGrade {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The look, or nothing to take it off.
        grade: Option<crate::media::Digest>,
    },
    /// Put a mask on a clip, or take one off.
    ///
    /// The shape itself rather than a handle to one, because a mask is
    /// geometry somebody dragged into place on *this* clip rather than
    /// content two projects could share. A grade is named by digest for the
    /// opposite reason: the same table in two projects is the same table.
    SetClipMask {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The shape, or nothing to take it off.
        mask: Option<crate::mask::Mask>,
    },
    /// Cut an item in two.
    SplitItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// How far into the item to cut.
        offset: i64,
    },
    /// Join an item with the one after it.
    JoinItems {
        /// Which track.
        track: usize,
        /// The first of the two.
        index: usize,
    },
    /// Add an empty track.
    AddTrack {
        /// Where in the track list.
        index: usize,
        /// What the track carries.
        kind: TrackKind,
    },
    /// Remove an empty track.
    RemoveTrack {
        /// Which track.
        index: usize,
    },
    /// Move a track's fader.
    SetTrackFader {
        /// Which track.
        track: usize,
        /// Where to move it to.
        fader: crate::track::Fader,
    },
    /// Animate a track's opacity over time, or stop animating it.
    ///
    /// [`None`] is not the same as a curve holding one: it is a track with no
    /// automation, which is what lets automation be switched off and back on
    /// without losing the shape somebody drew.
    SetTrackOpacity {
        /// Which track.
        track: usize,
        /// The curve, or nothing to stop reading one.
        opacity: Option<crate::curve::Curve>,
    },
    /// Drive a sound track's fader from a curve, or stop driving it.
    ///
    /// The curve's values are decibels, the same units the static fader is set
    /// in. [`None`] hands the track back to its static fader, which is not the
    /// same as a curve sitting at that value.
    SetTrackLevel {
        /// Which track.
        track: usize,
        /// The curve, or nothing to stop driving it.
        level: Option<crate::curve::Curve>,
    },
    /// Change one keyframe on one of a track's automation lanes.
    ///
    /// The operation is nested rather than spread across four more variants
    /// here, so that [`Edit::apply`] keeps one arm for all of them and the
    /// match that handles them is exhaustive over exactly those four.
    ///
    /// Adding to a lane with no automation starts one, and removing the last
    /// keyframe turns it off — which makes those two each other's exact
    /// inverse. Replacing a whole curve is still [`Edit::SetTrackOpacity`] and
    /// [`Edit::SetTrackLevel`]; this is what a keyframe *drag* is, and the
    /// difference matters to the journal: fifty drags of one keyframe should
    /// not be fifty copies of a thousand-keyframe curve.
    Keyframe {
        /// Which track.
        track: usize,
        /// Which of its two lanes.
        lane: crate::curve::Automation,
        /// What to do to it.
        operation: crate::curve::KeyframeEdit,
    },
    /// Put a dissolve on a cut.
    AddTransition {
        /// Which track.
        track: usize,
        /// The dissolve, and the cut it sits on.
        transition: crate::track::Transition,
    },
    /// Take a dissolve off a cut.
    RemoveTransition {
        /// Which track.
        track: usize,
        /// Which cut.
        boundary: usize,
    },
}

impl Edit {
    /// Apply this edit to a sequence and return the edit that undoes it.
    ///
    /// # Errors
    ///
    /// Any [`ModelStatus`] the change itself refuses. On a refusal the
    /// sequence is unchanged.
    #[expect(
        clippy::too_many_lines,
        reason = "a dispatch is as long as the number of things it dispatches"
    )]
    pub fn apply(&self, sequence: &mut Sequence) -> Result<Self> {
        // This is a dispatch table: one arm per edit, each three to six lines
        // of doing the thing and naming its inverse. Its length is the number
        // of operations the model supports, which is the point of it, and the
        // line lint is measuring the wrong property.
        //
        // Two things did come out of it, and they came out because they
        // deserved to rather than to satisfy a count: `retime` and `slip` are
        // the two arms that read before they write, and that shape is worth a
        // name. What has deliberately *not* happened is splitting the match
        // itself — a subset of a dispatch needs an arm for every variant it
        // does not handle, which is a branch nothing reaches and no test can
        // cover. `Edit::Keyframe` nests its four operations for exactly that
        // reason, and grouping the item edits the same way is the move if this
        // ever needs to shrink again.
        //
        // `expect` rather than `allow`, so that the day this drops back under
        // a hundred lines the compiler says so instead of leaving a stale
        // waiver behind.
        match self.clone() {
            Self::InsertItem { track, index, item } => {
                sequence.track_mut(track)?.insert(index, item)?;
                Ok(Self::RemoveItem { track, index })
            }
            Self::RemoveItem { track, index } => {
                let item = sequence.track_mut(track)?.remove(index)?;
                Ok(Self::InsertItem { track, index, item })
            }
            Self::SetItemDuration {
                track,
                index,
                duration,
            } => {
                let previous = retime(sequence.track_mut(track)?, index, duration)?;
                Ok(Self::SetItemDuration {
                    track,
                    index,
                    duration: previous,
                })
            }
            Self::SetClipSource {
                track,
                index,
                source_start,
            } => {
                let previous = slip(sequence.track_mut(track)?, index, source_start)?;
                Ok(Self::SetClipSource {
                    track,
                    index,
                    source_start: previous,
                })
            }
            Self::SetClipGrade {
                track,
                index,
                grade,
            } => {
                let previous = regrade(sequence.track_mut(track)?, index, grade)?;
                Ok(Self::SetClipGrade {
                    track,
                    index,
                    grade: previous,
                })
            }
            Self::SetClipMask { track, index, mask } => {
                let previous = remask(sequence.track_mut(track)?, index, mask)?;
                Ok(Self::SetClipMask {
                    track,
                    index,
                    mask: previous,
                })
            }
            Self::SplitItem {
                track,
                index,
                offset,
            } => {
                sequence.track_mut(track)?.split(index, offset)?;
                Ok(Self::JoinItems { track, index })
            }
            Self::JoinItems { track, index } => {
                let lane = sequence.track_mut(track)?;
                let offset = lane.item(index)?.duration().ticks();
                lane.join(index)?;
                Ok(Self::SplitItem {
                    track,
                    index,
                    offset,
                })
            }
            Self::AddTrack { index, kind } => {
                sequence.add_track(index, kind)?;
                Ok(Self::RemoveTrack { index })
            }
            Self::RemoveTrack { index } => {
                let kind = sequence.remove_track(index)?;
                Ok(Self::AddTrack { index, kind })
            }
            Self::AddTransition { track, transition } => {
                sequence.track_mut(track)?.add_transition(transition)?;
                Ok(Self::RemoveTransition {
                    track,
                    boundary: transition.boundary(),
                })
            }
            Self::RemoveTransition { track, boundary } => {
                let transition = sequence.track_mut(track)?.remove_transition(boundary)?;
                Ok(Self::AddTransition { track, transition })
            }
            Self::Keyframe {
                track,
                lane,
                operation,
            } => {
                let undo = sequence.track_mut(track)?.edit_keyframe(lane, operation)?;
                Ok(Self::Keyframe {
                    track,
                    lane,
                    operation: undo,
                })
            }
            Self::SetTrackOpacity { track, opacity } => {
                let previous = sequence.track_mut(track)?.set_opacity(opacity)?;
                Ok(Self::SetTrackOpacity {
                    track,
                    opacity: previous,
                })
            }
            Self::SetTrackLevel { track, level } => {
                let previous = sequence.track_mut(track)?.set_level(level)?;
                Ok(Self::SetTrackLevel {
                    track,
                    level: previous,
                })
            }
            Self::SetTrackFader { track, fader } => {
                let previous = sequence.track_mut(track)?.set_fader(fader);
                Ok(Self::SetTrackFader {
                    track,
                    fader: previous,
                })
            }
        }
    }
}

/// Put a look on a clip, giving back the look it had.
///
/// Out of [`Edit::apply`] for the same reason as [`retime`] and [`slip`]: it
/// reads before it writes.
fn regrade(
    lane: &mut crate::track::Track,
    index: usize,
    grade: Option<crate::media::Digest>,
) -> Result<Option<crate::media::Digest>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has nothing to grade. Accepting it and doing nothing would
        // make the inverse a lie: undoing would have nothing to put back and
        // would still claim to have.
        return Err(ModelStatus::NotAClip);
    };
    lane.replace(index, Item::Clip(clip.with_grade(grade)))?;
    Ok(clip.grade())
}

/// Put a mask on a clip, giving back the one it had.
fn remask(
    lane: &mut crate::track::Track,
    index: usize,
    mask: Option<crate::mask::Mask>,
) -> Result<Option<crate::mask::Mask>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has nothing to mask, and the same argument `regrade` makes
        // applies: accepting it and doing nothing would make the inverse a
        // lie.
        return Err(ModelStatus::NotAClip);
    };
    lane.replace(index, Item::Clip(clip.with_mask(mask)))?;
    Ok(clip.mask().cloned())
}

/// Change which part of its media a clip uses, giving back the part it used.
///
/// Out of [`Edit::apply`] for the same reason as [`retime`]: it reads before
/// it writes, which is the shape worth having a name, and the dispatch it came
/// from is long enough already.
fn slip(lane: &mut crate::track::Track, index: usize, source_start: i64) -> Result<i64> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    lane.replace(index, Item::Clip(clip.with_source(source_start)?))?;
    Ok(clip.source_start())
}

/// Change an item's length, giving back the length it had.
///
/// Extracted from [`Edit::apply`] because it is the one arm that reads before
/// it writes, and because the arm it came from had grown past what one
/// function should hold. Splitting the *match* instead would have needed an
/// arm for every variant the split does not handle — a branch nothing can
/// reach and no test can cover — so what comes out is a step rather than a
/// subset of the dispatch.
fn retime(lane: &mut crate::track::Track, index: usize, duration: Duration) -> Result<Duration> {
    let existing = lane.item(index)?.clone();
    let replacement = existing.with_duration(duration)?;
    Ok(lane.replace(index, replacement)?.duration())
}
