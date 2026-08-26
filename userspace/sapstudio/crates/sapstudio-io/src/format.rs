// SPDX-License-Identifier: GPL-3.0-only
//! The SapStudio project file.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPRJ"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       8     payload length in bytes, little-endian
//! 16      32    SHA-256 of the payload
//! 48      N     payload
//! ```
//!
//! Versioned from its first byte, length-prefixed, and digested (R-9.3). The
//! digest is what makes a half-written file detectable rather than loadable:
//! any change to any payload byte, and any change to the length, is refused by
//! name before a single field reaches the model.
//!
//! The payload is written by hand rather than derived. That is deliberate: the
//! format is the long-term custody of the user's work, so every field in it is
//! a decision someone made and can read here, and the decoder's bounds are the
//! model's own capacities rather than whatever a derive happened to allow.
//!
//! History is not saved. Undo is a property of a session, not of a project; a
//! file that carried its own history would make "open the file" and "open the
//! file and undo twice" two different projects with one name.

use alloc::vec::Vec;

use sapstudio_core::{Digest, Duration, Instant, Rational, Timebase};
use sapstudio_model::curve::MAX_KEYFRAMES;
use sapstudio_model::mask::{MAX_CORNERS, Mask};
use sapstudio_model::media::{Location, MAX_LOCATION_BYTES};
use sapstudio_model::project::{MAX_MEDIA, MAX_SEQUENCES};
use sapstudio_model::sequence::MAX_TRACKS_PER_SEQUENCE;
use sapstudio_model::track::MAX_ITEMS_PER_TRACK;
use sapstudio_model::{
    Clip, Curve, Edit, Fader, Interpolation, Item, Keyframe, MediaAsset, MediaId, Project,
    TrackKind, Transition, TransitionKind, Wipe,
};

use crate::bytes::{Reader, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every project file begins with.
pub const MAGIC: [u8; 4] = *b"SPRJ";

/// The format this build writes.
/// Six. Version two added the fader, three the dissolves, four a picture
/// track's opacity over time, five a sound track's fader over time, and six a
/// clip's grade.
///
/// Version one had no fader. A project written by version one and read as
/// version two would have its faders read out of whatever followed them, which
/// is why the number moves for every change to what the bytes mean and not
/// only for changes that break (R-1.2). A clip's grade is written after its
/// length, so a version-five file read as version six would find a flag byte
/// in the next item's tag.
pub const FORMAT_VERSION: u16 = 10;

/// How long the fixed header is.
pub const HEADER_BYTES: usize = 48;

/// The largest payload this format accepts.
///
/// Sixteen mebibytes: a project is structure, not media, and a structural
/// description larger than this is a generated one rather than an edited one.
pub const MAX_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;

/// How an item is tagged in the payload.
const TAG_CLIP: u8 = 0;
const TAG_GAP: u8 = 1;

/// How a track's kind is tagged in the payload.
const KIND_VIDEO: u8 = 0;
const KIND_AUDIO: u8 = 1;

/// A transition that cross-fades the whole frame.
const KIND_DISSOLVE: u8 = 0;

/// A transition that sweeps a straight edge across it.
const KIND_WIPE: u8 = 1;

/// Encode a project as a complete file.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`], [`IoStatus::OutOfMemory`], or a model
/// refusal if the project holds something the format cannot describe.
pub fn encode(project: &Project) -> Result<Vec<u8>> {
    let payload = encode_payload(project)?;
    let digest = Digest::of(&payload);

    let mut file = Writer::new(HEADER_BYTES + MAX_PAYLOAD_BYTES);
    file.bytes(&MAGIC)?;
    file.u16(FORMAT_VERSION)?;
    file.u16(0)?;
    file.u64(payload.len() as u64)?;
    file.bytes(digest.bytes())?;
    file.bytes(&payload)?;
    Ok(file.finish())
}

/// Decode a complete file into a project.
///
/// Nothing is published until every byte has been accounted for: the header,
/// the digest, the whole structure, and the absence of anything after it.
///
/// # Errors
///
/// Any [`IoStatus`]. On a refusal nothing is returned, so a caller cannot act
/// on a partly decoded project (R-1.4).
pub fn decode(file: &[u8]) -> Result<Project> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    let mut header = Reader::new(&file[..HEADER_BYTES]);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotAProjectFile);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let declared = header.u64()?;
    let expected = Digest::new(header.digest_bytes()?);

    let declared = usize::try_from(declared).map_err(|_| IoStatus::PayloadTooLarge)?;
    if declared > MAX_PAYLOAD_BYTES {
        return Err(IoStatus::PayloadTooLarge);
    }
    let available = file.len() - HEADER_BYTES;
    if declared > available {
        return Err(IoStatus::TruncatedPayload);
    }
    if declared < available {
        return Err(IoStatus::TrailingBytes);
    }

    let payload = &file[HEADER_BYTES..];
    if Digest::of(payload) != expected {
        return Err(IoStatus::DigestMismatch);
    }
    decode_payload(payload)
}

/// Encode just the payload.
fn encode_payload(project: &Project) -> Result<Vec<u8>> {
    let mut writer = Writer::new(MAX_PAYLOAD_BYTES);

    // Media are written in slot order and referred to afterwards by their
    // position in this list. Generational identifiers are a runtime idea: they
    // describe which occupancy of a slot a reference meant, and a file has no
    // slots and no occupancies.
    let media: Vec<(MediaId, &MediaAsset)> = project.media().iter().collect();
    writer.u32(u32::try_from(media.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, asset) in &media {
        writer.bytes(asset.digest().bytes())?;
        write_timebase(&mut writer, asset.timebase())?;
        writer.i64(asset.duration().ticks())?;
        // A length and then the bytes, uninterpreted. A path is whatever the
        // platform says it is, and this format does not know which platform
        // wrote it -- so it carries the hint and declines to read it.
        match asset.location() {
            None => writer.u32(0)?,
            Some(location) => {
                writer
                    .u32(u32::try_from(location.bytes().len()).map_err(|_| IoStatus::TooMany)?)?;
                writer.bytes(location.bytes())?;
            }
        }
    }

    let sequences: Vec<_> = project.sequences().iter().collect();
    writer.u32(u32::try_from(sequences.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, sequence) in &sequences {
        write_timebase(&mut writer, sequence.timebase())?;
        writer.u32(u32::try_from(sequence.track_count()).map_err(|_| IoStatus::TooMany)?)?;
        for track in sequence.tracks() {
            writer.u8(match track.kind() {
                TrackKind::Video => KIND_VIDEO,
                TrackKind::Audio => KIND_AUDIO,
            })?;
            write_fader(&mut writer, track.fader())?;
            writer.u32(u32::try_from(track.len()).map_err(|_| IoStatus::TooMany)?)?;
            for item in track.items() {
                match item {
                    Item::Clip(clip) => {
                        writer.u8(TAG_CLIP)?;
                        let index = media
                            .iter()
                            .position(|(id, _)| *id == clip.media())
                            .ok_or(IoStatus::MediaIndexOutOfRange)?;
                        writer.u32(u32::try_from(index).map_err(|_| IoStatus::TooMany)?)?;
                        writer.i64(clip.source_start())?;
                        writer.i64(clip.duration().ticks())?;
                        // A flag rather than a fixed thirty-two bytes, because
                        // most clips have no grade and an all-zero digest is a
                        // real digest of something rather than a spare value
                        // to spend on "none".
                        match clip.mask() {
                            None => writer.u8(MASK_NONE)?,
                            Some(mask) => {
                                writer.u8(MASK_PRESENT)?;
                                writer.u8(u8::from(mask.is_inverted()))?;
                                writer.u32(
                                    u32::try_from(mask.corners().len())
                                        .map_err(|_| IoStatus::TooMany)?,
                                )?;
                                for corner in mask.corners() {
                                    write_rational(&mut writer, corner.0)?;
                                    write_rational(&mut writer, corner.1)?;
                                }
                            }
                        }
                        match clip.grade() {
                            None => writer.u8(GRADE_NONE)?,
                            Some(grade) => {
                                writer.u8(GRADE_PRESENT)?;
                                writer.bytes(grade.bytes())?;
                            }
                        }
                    }
                    Item::Gap(duration) => {
                        writer.u8(TAG_GAP)?;
                        writer.i64(duration.ticks())?;
                    }
                }
            }
            // Transitions come after the items rather than before, because
            // they name item indices and a reader cannot check one against a
            // track it has not read yet.
            let held = track.transitions();
            writer.u32(u32::try_from(held.len()).map_err(|_| IoStatus::TooMany)?)?;
            for transition in held {
                writer.u32(u32::try_from(transition.boundary()).map_err(|_| IoStatus::TooMany)?)?;
                writer.i64(transition.duration().ticks())?;
                // A tag before the parameters, so that a later kind adds a
                // tag rather than changing what the bytes after the duration
                // mean. Version six wrote no tag at all because there was one
                // kind, which is exactly the shape that makes adding a second
                // a format break rather than an extension.
                match transition.kind() {
                    TransitionKind::Dissolve => writer.u8(KIND_DISSOLVE)?,
                    TransitionKind::Wipe(wipe) => {
                        writer.u8(KIND_WIPE)?;
                        write_rational(&mut writer, wipe.across())?;
                        write_rational(&mut writer, wipe.down())?;
                        write_rational(&mut writer, wipe.softness())?;
                    }
                }
            }
            write_curve(&mut writer, track.opacity())?;
            write_curve(&mut writer, track.level())?;
        }
    }
    Ok(writer.finish())
}

/// Decode just the payload.
fn decode_payload(payload: &[u8]) -> Result<Project> {
    let mut reader = Reader::new(payload);
    let mut project = Project::new();

    let media_count = bounded(reader.u32()?, MAX_MEDIA)?;
    let mut media = Vec::new();
    media
        .try_reserve(media_count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..media_count {
        let digest = Digest::new(reader.digest_bytes()?);
        let timebase = read_timebase(&mut reader)?;
        let ticks = reader.i64()?;
        let duration = Duration::new(ticks, timebase)?;
        let length = bounded(reader.u32()?, MAX_LOCATION_BYTES)?;
        let location = if length == 0 {
            None
        } else {
            Some(Location::new(reader.take(length)?).map_err(IoStatus::Model)?)
        };
        // One asset per digest, so a file that lists one twice is not a file
        // this encoder could have written. `add_media` would quietly hand back
        // the identifier it already had, and every clip indexing the second
        // record would then point at the first -- which is a different
        // programme, arrived at silently. Refused instead.
        if project.find_media(digest).is_some() {
            return Err(IoStatus::DuplicateMedia);
        }
        let asset = MediaAsset::new(digest, timebase, duration)?.with_location(location);
        media.push(project.add_media(asset)?);
    }

    let sequence_count = bounded(reader.u32()?, MAX_SEQUENCES)?;
    for _ in 0..sequence_count {
        let timebase = read_timebase(&mut reader)?;
        let id = project.add_sequence(timebase)?;
        let track_count = bounded(reader.u32()?, MAX_TRACKS_PER_SEQUENCE)?;
        for track in 0..track_count {
            let tag = reader.u8()?;
            let kind = match tag {
                KIND_VIDEO => TrackKind::Video,
                KIND_AUDIO => TrackKind::Audio,
                other => return Err(IoStatus::UnknownTrackKind(other)),
            };
            project.apply(id, Edit::AddTrack { index: track, kind })?;
            let fader = read_fader(&mut reader)?;
            if fader != Fader::UNITY {
                // Applied as an edit like everything else, so that loading a
                // project uses the one way in and nothing can set a value the
                // model would have refused.
                project.apply(id, Edit::SetTrackFader { track, fader })?;
            }
            let item_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
            for index in 0..item_count {
                let item = read_item(&mut reader, timebase, &media)?;
                project.apply(id, Edit::InsertItem { track, index, item })?;
            }
            let transition_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
            for _ in 0..transition_count {
                let boundary = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
                let duration = Duration::new(reader.i64()?, timebase).map_err(IoStatus::Time)?;
                let tag = reader.u8()?;
                let kind = match tag {
                    KIND_DISSOLVE => TransitionKind::Dissolve,
                    KIND_WIPE => {
                        let across = read_rational(&mut reader)?;
                        let down = read_rational(&mut reader)?;
                        let softness = read_rational(&mut reader)?;
                        TransitionKind::Wipe(
                            Wipe::soft(across, down, softness).map_err(IoStatus::Model)?,
                        )
                    }
                    other => return Err(IoStatus::UnknownTransitionTag(other)),
                };
                let transition =
                    Transition::of(boundary, duration, kind).map_err(IoStatus::Model)?;
                // Through the edit, like everything else, so a file cannot set
                // a dissolve the model itself would have refused — one on a
                // gap, one longer than its clips, or one whose incoming side
                // has no room for handles.
                project.apply(id, Edit::AddTransition { track, transition })?;
            }
            if let Some(opacity) = read_curve(&mut reader, timebase)? {
                // Through the edit, like everything else, so a file cannot set
                // a curve the model itself would have refused — one on the
                // wrong kind of track, or one counted in another timebase.
                project.apply(
                    id,
                    Edit::SetTrackOpacity {
                        track,
                        opacity: Some(opacity),
                    },
                )?;
            }
            if let Some(level) = read_curve(&mut reader, timebase)? {
                project.apply(
                    id,
                    Edit::SetTrackLevel {
                        track,
                        level: Some(level),
                    },
                )?;
            }
        }
    }

    if !reader.is_finished() {
        return Err(IoStatus::TrailingBytes);
    }

    // Loading is not editing: a freshly opened project has nothing to undo.
    // The edits above are how the structure is built, not a history the user
    // performed, and offering to undo them would offer to undo the file.
    project.forget_history();
    Ok(project)
}

/// One item.
fn read_item(reader: &mut Reader<'_>, timebase: Timebase, media: &[MediaId]) -> Result<Item> {
    match reader.u8()? {
        TAG_CLIP => {
            let index = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
            let id = *media.get(index).ok_or(IoStatus::MediaIndexOutOfRange)?;
            let source_start = reader.i64()?;
            let duration = Duration::new(reader.i64()?, timebase)?;
            let mask = match reader.u8()? {
                MASK_NONE => None,
                MASK_PRESENT => {
                    let inverted = match reader.u8()? {
                        0 => false,
                        1 => true,
                        other => return Err(IoStatus::UnknownMaskTag(other)),
                    };
                    let count = bounded(reader.u32()?, MAX_CORNERS)?;
                    let mut corners = Vec::new();
                    for _ in 0..count {
                        let x = read_rational(reader)?;
                        let y = read_rational(reader)?;
                        corners.push((x, y));
                    }
                    // Through the model's own constructor, like everything
                    // else, so a file cannot hold a shape the model would have
                    // refused -- a concave one, or one with no area.
                    Some(
                        Mask::new(corners)
                            .map_err(IoStatus::Model)?
                            .with_inversion(inverted),
                    )
                }
                other => return Err(IoStatus::UnknownMaskTag(other)),
            };
            let grade = match reader.u8()? {
                GRADE_NONE => None,
                GRADE_PRESENT => Some(Digest::new(reader.digest_bytes()?)),
                other => return Err(IoStatus::UnknownGradeTag(other)),
            };
            Ok(Item::Clip(
                Clip::new(id, source_start, duration)?
                    .with_grade(grade)
                    .with_mask(mask),
            ))
        }
        TAG_GAP => {
            let duration = Duration::new(reader.i64()?, timebase)?;
            Ok(Item::gap(duration)?)
        }
        other => Err(IoStatus::UnknownItemTag(other)),
    }
}

/// A clip with no shape on it.
const MASK_NONE: u8 = 0;

/// A clip whose shape follows.
const MASK_PRESENT: u8 = 1;

/// A clip with no look on it.
const GRADE_NONE: u8 = 0;
/// A clip whose look follows as thirty-two bytes.
const GRADE_PRESENT: u8 = 1;

/// How the interpolations are tagged.
///
/// The numbers are part of the format and may never be reassigned: a tag that
/// changed meaning would turn every saved ease into a hold, silently (R-1.2).
const HOW_HOLD: u8 = 1;
/// A straight run to the next keyframe.
const HOW_LINEAR: u8 = 2;
/// A cubic Bézier, followed by its four handle components.
const HOW_EASE: u8 = 3;

/// Write one of a track's automation curves, or the absence of one.
///
/// A count of nought means no automation, which is not the same as a curve
/// holding one: a track with no automation has none to read, and the model
/// keeps them apart so automation can be switched off and back on without
/// losing the shape somebody drew.
fn write_curve(writer: &mut Writer, curve: Option<&Curve>) -> Result<()> {
    let Some(curve) = curve else {
        return writer.u32(0);
    };
    let keyframes = curve.keyframes();
    writer.u32(u32::try_from(keyframes.len()).map_err(|_| IoStatus::TooMany)?)?;
    for keyframe in keyframes {
        writer.i64(keyframe.at().ticks())?;
        write_rational(writer, keyframe.value())?;
        match keyframe.interpolation() {
            Interpolation::Hold => writer.u8(HOW_HOLD)?,
            Interpolation::Linear => writer.u8(HOW_LINEAR)?,
            Interpolation::Ease {
                out_x,
                out_y,
                in_x,
                in_y,
            } => {
                writer.u8(HOW_EASE)?;
                for handle in [out_x, out_y, in_x, in_y] {
                    write_rational(writer, handle)?;
                }
            }
        }
    }
    Ok(())
}

/// Read one of a track's automation curves, if it has one.
fn read_curve(reader: &mut Reader<'_>, timebase: Timebase) -> Result<Option<Curve>> {
    let count = bounded(reader.u32()?, MAX_KEYFRAMES)?;
    if count == 0 {
        return Ok(None);
    }
    let mut keyframes = Vec::new();
    keyframes
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..count {
        let at = Instant::new(reader.i64()?, timebase);
        let value = read_rational(reader)?;
        let tag = reader.u8()?;
        let how = match tag {
            HOW_HOLD => Interpolation::Hold,
            HOW_LINEAR => Interpolation::Linear,
            HOW_EASE => Interpolation::Ease {
                out_x: read_rational(reader)?,
                out_y: read_rational(reader)?,
                in_x: read_rational(reader)?,
                in_y: read_rational(reader)?,
            },
            other => return Err(IoStatus::UnknownInterpolationTag(other)),
        };
        // Through the model's own constructor, so a file cannot carry a handle
        // that folds the curve back on itself.
        keyframes.push(Keyframe::new(at, value, how).map_err(IoStatus::Model)?);
    }
    Ok(Some(Curve::new(keyframes).map_err(IoStatus::Model)?))
}

/// Write a rational as its two halves.
///
/// Both, rather than a single scaled integer, because the whole point of the
/// type is that a third is a third: writing it as a decimal would be the one
/// place in the project file where an exact number stopped being one.
fn write_rational(writer: &mut Writer, value: Rational) -> Result<()> {
    writer.i64(value.numerator())?;
    writer.i64(value.denominator())
}

/// Read a rational, refusing one the core type would not build.
fn read_rational(reader: &mut Reader<'_>) -> Result<Rational> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Rational::new(numerator, denominator).map_err(IoStatus::Time)
}

/// How a muted fader is tagged, as against one at a level.
const FADER_MUTED: u8 = 0;

/// How a fader at a level is tagged.
const FADER_LEVEL: u8 = 1;

/// Write a track's fader.
///
/// A tag then a ratio, because "off" is not a point on the decibel scale and
/// encoding it as a very small number would make it one — a reader could then
/// not tell a muted track from a track turned all the way down, and unmuting
/// would not restore the level.
fn write_fader(writer: &mut Writer, fader: Fader) -> Result<()> {
    match fader.decibels() {
        None => {
            writer.u8(FADER_MUTED)?;
            writer.i64(0)?;
            writer.i64(1)
        }
        Some(decibels) => {
            writer.u8(FADER_LEVEL)?;
            writer.i64(decibels.numerator())?;
            writer.i64(decibels.denominator())
        }
    }
}

/// Read a track's fader, refusing a level the model would not accept.
fn read_fader(reader: &mut Reader<'_>) -> Result<Fader> {
    let tag = reader.u8()?;
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    match tag {
        FADER_MUTED => Ok(Fader::MUTED),
        FADER_LEVEL => {
            let level = Rational::new(numerator, denominator).map_err(IoStatus::Time)?;
            Ok(Fader::at(level)?)
        }
        other => Err(IoStatus::UnknownFaderTag(other)),
    }
}

fn write_timebase(writer: &mut Writer, timebase: Timebase) -> Result<()> {
    writer.i64(timebase.rate().numerator())?;
    writer.i64(timebase.rate().denominator())
}

/// Read a timebase, refusing anything that is not a rate.
fn read_timebase(reader: &mut Reader<'_>) -> Result<Timebase> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Ok(Timebase::new(Rational::new(numerator, denominator)?)?)
}

/// Refuse a declared count the model could not hold anyway.
///
/// This is the check that keeps a hostile file from asking for four billion
/// tracks and being refused only after the allocator has tried (R-11.2).
fn bounded(declared: u32, limit: usize) -> Result<usize> {
    let count = usize::try_from(declared).map_err(|_| IoStatus::TooMany)?;
    if count > limit {
        return Err(IoStatus::TooMany);
    }
    Ok(count)
}
