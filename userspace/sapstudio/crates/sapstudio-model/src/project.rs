// SPDX-License-Identifier: GPL-3.0-only
//! The project: media, sequences, and the history over them.
//!
//! This is the single source of truth (R-9.1). Everything else in SapStudio —
//! caches, rendered frames, waveform overviews, interface state — is derived
//! from it and can be thrown away.

use sapstudio_core::Timebase;

use crate::edit::Edit;
use crate::item::Item;
use crate::journal::EditJournal;
use crate::media::{MediaAsset, MediaId};
use crate::sequence::{Sequence, SequenceId};
use crate::status::{ModelStatus, Result};
use crate::store::Store;

/// How many media assets one project may hold.
pub const MAX_MEDIA: usize = 65_536;

/// How many sequences one project may hold.
pub const MAX_SEQUENCES: usize = 1024;

/// A whole editing project.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Project {
    media: Store<MediaAsset>,
    sequences: Store<Sequence>,
    history: EditJournal,
}

impl Default for Project {
    fn default() -> Self {
        Self::new()
    }
}

impl Project {
    /// An empty project.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            media: Store::new(MAX_MEDIA),
            sequences: Store::new(MAX_SEQUENCES),
            history: EditJournal::new(),
        }
    }

    /// The media library.
    #[must_use]
    pub const fn media(&self) -> &Store<MediaAsset> {
        &self.media
    }

    /// The sequences.
    #[must_use]
    pub const fn sequences(&self) -> &Store<Sequence> {
        &self.sequences
    }

    /// The history.
    #[must_use]
    pub const fn history(&self) -> &EditJournal {
        &self.history
    }

    /// Add a media asset to the library, or find the one already there.
    ///
    /// **One asset per digest.** Media is content-addressed, so the same bytes
    /// are the same asset however many times somebody opens the file — adding
    /// it again gives back the identifier it already has rather than a second
    /// one naming the same content.
    ///
    /// That is not a convenience. Two identifiers for one digest quietly
    /// falsified the conform round trip: an export writes the digest, an
    /// import looks it up, and with two candidates it finds the first — so a
    /// sequence cutting the same footage under two identifiers came back
    /// pointing at one of them, with nothing reported as lost. The theorem was
    /// stated three milestones before the case that breaks it was tried.
    ///
    /// The location hint is deliberately *not* part of the comparison, and the
    /// existing one is kept. The same content found in a second place is the
    /// same content; moving the hint is [`Edit::SetMediaLocation`]'s job, and
    /// doing it here would rewrite a project's records as a side effect of
    /// opening a file.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaContradiction`] if an asset with this digest is
    /// already held and describes it differently — the same bytes cannot be
    /// two lengths — and [`ModelStatus::CapacityExhausted`] or
    /// [`ModelStatus::OutOfMemory`].
    pub fn add_media(&mut self, asset: MediaAsset) -> Result<MediaId> {
        if let Some(held) = self.find_media(asset.digest()) {
            let existing = self.media.get(held)?;
            if existing.timebase() != asset.timebase() || existing.duration() != asset.duration() {
                return Err(ModelStatus::MediaContradiction);
            }
            return Ok(held);
        }
        self.media.insert(asset)
    }

    /// Which asset holds this content, if any.
    #[must_use]
    pub fn find_media(&self, digest: crate::media::Digest) -> Option<MediaId> {
        self.media
            .iter()
            .find(|(_, asset)| asset.digest() == digest)
            .map(|(id, _)| id)
    }

    /// Say where an asset's bytes were last seen, giving back the old hint.
    ///
    /// **Relinking is this and nothing else.** A hint moves; identity does
    /// not. Pointing a clip at *different* bytes is different media, and the
    /// digest says so rather than a flag — so there is no operation here that
    /// swaps one piece of content for another while keeping its name, because
    /// that is the operation that silently changes what a programme is.
    ///
    /// This is **not in the undo journal**, and that is a limitation rather
    /// than a decision. The journal applies edits to a *sequence*, and the
    /// media library belongs to the project; making a media change undoable
    /// means the journal becoming project-level, which is a larger change than
    /// the hint is worth. The previous hint is returned so a caller can put it
    /// back, which is the whole of what undo would do.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a stale or unknown identifier.
    pub fn set_media_location(
        &mut self,
        id: MediaId,
        location: Option<crate::media::Location>,
    ) -> Result<Option<crate::media::Location>> {
        let asset = self.media.get_mut(id)?;
        let previous = asset.location().cloned();
        *asset = asset.with_location(location);
        Ok(previous)
    }

    /// Remove a media asset, if nothing cuts from it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaInUse`] if any sequence still refers to it, or an
    /// identifier refusal.
    pub fn remove_media(&mut self, id: MediaId) -> Result<MediaAsset> {
        for (_, sequence) in self.sequences.iter() {
            for track in sequence.tracks() {
                for item in track.items() {
                    if matches!(item, Item::Clip(clip) if clip.media() == id) {
                        return Err(ModelStatus::MediaInUse);
                    }
                }
            }
        }
        self.media.remove(id)
    }

    /// Add an empty sequence at a rate.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::CapacityExhausted`] or [`ModelStatus::OutOfMemory`].
    pub fn add_sequence(&mut self, timebase: Timebase) -> Result<SequenceId> {
        self.sequences.insert(Sequence::new(timebase))
    }

    /// Borrow a sequence.
    ///
    /// # Errors
    ///
    /// An identifier refusal.
    pub fn sequence(&self, id: SequenceId) -> Result<&Sequence> {
        self.sequences.get(id)
    }

    /// Apply an edit to a sequence and record it in the project's history.
    ///
    /// Every clip the edit introduces is checked against the library first:
    /// the media must exist, share the sequence's timebase, and contain the
    /// whole source range the clip asks for. A refusal leaves both the
    /// sequence and the history untouched.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownMedia`], [`ModelStatus::MediaTimebaseMismatch`],
    /// [`ModelStatus::SourceAfterEnd`], or whatever the edit itself refuses.
    pub fn apply(&mut self, id: SequenceId, edit: Edit) -> Result<()> {
        self.validate(id, &edit)?;
        let sequence = self.sequences.get_mut(id)?;
        self.history.apply(sequence, edit)
    }

    /// Undo the most recent edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn undo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.undo(sequence)
    }

    /// Redo the most recently undone edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn redo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.redo(sequence)
    }

    /// Discard the undo history, keeping the project exactly as it is.
    ///
    /// Loading a file uses this. The edits that build a project out of its
    /// saved structure are how the structure is expressed, not something the
    /// user did, and offering to undo them would offer to undo the file
    /// itself. A freshly opened project has nothing to undo, which is what
    /// every editor means by opening one.
    pub fn forget_history(&mut self) {
        self.history = EditJournal::new();
    }

    /// Check an edit against the media library before it is applied.
    ///
    /// Three edits can put a clip's source range outside its media: inserting
    /// a clip, lengthening one, and slipping one. Each is checked here, on the
    /// values the edit would produce, before anything changes. Removals,
    /// splits, joins, and track edits cannot introduce a reference or widen a
    /// range, so they have nothing to check.
    fn validate(&self, id: SequenceId, edit: &Edit) -> Result<()> {
        let sequence = self.sequences.get(id)?;
        match edit {
            Edit::InsertItem { item, .. } => {
                let Item::Clip(clip) = item else {
                    return Ok(());
                };
                self.check_source(
                    sequence,
                    clip.media(),
                    clip.source_start(),
                    clip.duration().ticks(),
                )
            }
            Edit::SetItemDuration {
                track,
                index,
                duration,
            } => {
                let Item::Clip(clip) = sequence.track(*track)?.item(*index)? else {
                    return Ok(());
                };
                self.check_source(
                    sequence,
                    clip.media(),
                    clip.source_start(),
                    duration.ticks(),
                )
            }
            Edit::SetClipSource {
                track,
                index,
                source_start,
            } => {
                let Item::Clip(clip) = sequence.track(*track)?.item(*index)? else {
                    return Ok(());
                };
                self.check_source(
                    sequence,
                    clip.media(),
                    *source_start,
                    clip.duration().ticks(),
                )
            }
            _ => Ok(()),
        }
    }

    /// Whether a source range lies inside the media it names.
    fn check_source(
        &self,
        sequence: &Sequence,
        media: MediaId,
        source_start: i64,
        length: i64,
    ) -> Result<()> {
        let asset = self
            .media
            .get(media)
            .map_err(|_| ModelStatus::UnknownMedia)?;
        if asset.timebase() != sequence.timebase() {
            return Err(ModelStatus::MediaTimebaseMismatch);
        }
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        let end = source_start
            .checked_add(length)
            .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?;
        if end > asset.duration().ticks() {
            return Err(ModelStatus::SourceAfterEnd);
        }
        Ok(())
    }
}
