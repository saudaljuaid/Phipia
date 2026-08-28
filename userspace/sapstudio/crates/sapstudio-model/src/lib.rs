// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "SapStudio and Sapote are product names, not identifiers"
)]
//! The SapStudio project model.
//!
//! A project is media, sequences, and history. A sequence is tracks; a track
//! is items; an item is a clip or a gap. Positions are not stored: an item's
//! place on its track is the sum of the lengths before it, so overlapping
//! items and unaccounted holes are not representable rather than merely
//! rejected.
//!
//! An [`Edit`] is a value that carries its own inverse, which is what makes
//! [`EditJournal`] a list of pairs instead of a special mode, and what makes
//! "undo everything reproduces the project exactly" a property a test can
//! generate thousands of cases for.
//!
//! There is no floating point here, no `unsafe`, and no allocation that is not
//! both bounded by a named policy constant and fallible (R-5.1, R-5.2).

extern crate alloc;

mod bounded;

pub mod curve;
pub mod edit;
pub mod item;
pub mod journal;
pub mod marker;
pub mod mask;
pub mod media;
pub mod project;
pub mod sequence;
pub mod stack;
pub mod status;
pub mod store;
pub mod title;
pub mod track;
pub mod transform;

pub use curve::{Automation, Curve, Interpolation, Keyframe, KeyframeEdit, MAX_KEYFRAMES};
pub use edit::Edit;
pub use item::{Clip, Item, Playback};
pub use journal::EditJournal;
pub use marker::{MAX_MARKER_TEXT, MAX_MARKERS_PER_SEQUENCE, Marker};
pub use mask::{MAX_CORNERS, Mask};
pub use media::{Digest, Location, MAX_LOCATION_BYTES, MediaAsset, MediaId, MediaSource};
pub use project::Project;
pub use sequence::{MAX_TRACKS_PER_SEQUENCE, Sequence, SequenceId, TrackSet};
pub use stack::{Graded, Lane, Layer, Revealed};
pub use status::{ModelStatus, Result};
pub use store::Store;
pub use title::{Alignment, Ink, MAX_TITLE_LINES, MAX_TITLE_TEXT, Title};
pub use track::{
    Fader, MAXIMUM_DECIBELS, MINIMUM_DECIBELS, Track, TrackKind, Transition, TransitionKind, Wipe,
};
pub use transform::{Motion, Resampling, Transform, Turn};
