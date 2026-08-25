// SPDX-License-Identifier: GPL-3.0-only
//! An editable programme: a timebase and its tracks.

use alloc::vec::Vec;

use sapstudio_core::{Duration, Id, Timebase};

use crate::bounded::insert_bounded;
use crate::status::{ModelStatus, Result};
use crate::track::{Track, TrackKind};

/// How many tracks one sequence may hold. A policy bound: past this a
/// sequence is a rendering problem rather than an editing one.
pub const MAX_TRACKS_PER_SEQUENCE: usize = 128;

/// A cut: everything the timeline shows, at one rate.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sequence {
    timebase: Timebase,
    tracks: Vec<Track>,
}

impl Sequence {
    /// An empty sequence at a rate.
    #[must_use]
    pub const fn new(timebase: Timebase) -> Self {
        Self {
            timebase,
            tracks: Vec::new(),
        }
    }

    /// The rate everything in this sequence is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// How many tracks there are.
    #[must_use]
    pub fn track_count(&self) -> usize {
        self.tracks.len()
    }

    /// The tracks, in order.
    #[must_use]
    pub fn tracks(&self) -> &[Track] {
        &self.tracks
    }

    /// One track.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] if the index names nothing.
    pub fn track(&self, index: usize) -> Result<&Track> {
        self.tracks.get(index).ok_or(ModelStatus::UnknownTrack)
    }

    /// One track, for modification.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] if the index names nothing.
    pub fn track_mut(&mut self, index: usize) -> Result<&mut Track> {
        self.tracks.get_mut(index).ok_or(ModelStatus::UnknownTrack)
    }

    /// Add a track at an index, moving the ones after it down.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] for an index past the end,
    /// [`ModelStatus::CapacityExhausted`], or [`ModelStatus::OutOfMemory`].
    pub fn add_track(&mut self, index: usize, kind: TrackKind) -> Result<()> {
        if index > self.tracks.len() {
            return Err(ModelStatus::UnknownTrack);
        }
        let track = Track::new(kind, self.timebase);
        insert_bounded(&mut self.tracks, index, track, MAX_TRACKS_PER_SEQUENCE).map_err(|status| {
            match status {
                // `insert_bounded` speaks about items; here the thing being
                // indexed is a track, and the refusal should say so.
                ModelStatus::UnknownItem => ModelStatus::UnknownTrack,
                other => other,
            }
        })
    }

    /// Remove an empty track.
    ///
    /// A track with items on it is refused rather than emptied, so that
    /// removing a track can never destroy work in one step (R-9.1).
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] or [`ModelStatus::TrackNotEmpty`].
    pub fn remove_track(&mut self, index: usize) -> Result<TrackKind> {
        let track = self.track(index)?;
        if !track.is_empty() {
            return Err(ModelStatus::TrackNotEmpty);
        }
        let kind = track.kind();
        self.tracks.remove(index);
        Ok(kind)
    }

    /// How long the sequence is: as long as its longest track.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn duration(&self) -> Result<Duration> {
        let mut longest = Duration::zero(self.timebase);
        for track in &self.tracks {
            let track_duration = track.duration()?;
            if track_duration.compare(longest)? == core::cmp::Ordering::Greater {
                longest = track_duration;
            }
        }
        Ok(longest)
    }
}

/// A reference to a sequence in a project.
pub type SequenceId = Id<Sequence>;
