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
pub mod mask;
pub mod media;
pub mod project;
pub mod sequence;
pub mod stack;
pub mod status;
pub mod store;
pub mod track;

pub use curve::{Automation, Curve, Interpolation, Keyframe, KeyframeEdit, MAX_KEYFRAMES};
pub use edit::Edit;
pub use item::{Clip, Item};
pub use journal::EditJournal;
pub use mask::{MAX_CORNERS, Mask};
pub use media::{Digest, Location, MAX_LOCATION_BYTES, MediaAsset, MediaId};
pub use project::Project;
pub use sequence::{Sequence, SequenceId};
pub use stack::{Lane, Layer, Revealed};
pub use status::{ModelStatus, Result};
pub use store::Store;
pub use track::{
    Fader, MAXIMUM_DECIBELS, MINIMUM_DECIBELS, Track, TrackKind, Transition, TransitionKind, Wipe,
};
