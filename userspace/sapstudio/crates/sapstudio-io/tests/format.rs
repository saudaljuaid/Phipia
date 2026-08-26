// SPDX-License-Identifier: GPL-3.0-only
//! The project file: what it accepts, and everything it refuses.
//!
//! The sweeps at the end are this crate's fuzzing until `cargo-fuzz` can be
//! brought through the dependency gate (R-11.3). They are deterministic, they
//! run in CI, and between them they put every byte of a real file, every
//! prefix of it, and a few hundred thousand bytes of garbage through the
//! decoder. The decoder may refuse; it may not misbehave.

use sapstudio_core::{Digest, Duration, Instant, Rational, Timebase};
use sapstudio_io::{FORMAT_VERSION, HEADER_BYTES, IoStatus, MAGIC, decode, encode};
use sapstudio_model::curve::{Curve, Interpolation, Keyframe};
use sapstudio_model::{Clip, Edit, Item, MediaAsset, Project, TrackKind};

const RATE: Timebase = Timebase::NTSC_30;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

/// A project with two assets, two sequences, and a mixture of items.
fn sample() -> Project {
    let mut project = Project::new();
    let first = project
        .add_media(MediaAsset::new(Digest::of(b"first"), RATE, frames(17_982)).expect("an asset"))
        .expect("room");
    let second = project
        .add_media(MediaAsset::new(Digest::of(b"second"), RATE, frames(3_000)).expect("an asset"))
        .expect("room");

    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 1,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(first, 300, frames(1_798)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(48)).expect("a gap"),
            },
        )
        .expect("a gap");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 2,
                item: Item::Clip(Clip::new(second, 0, frames(900)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 1,
                index: 0,
                item: Item::Clip(Clip::new(first, 0, frames(2_746)).expect("a clip")),
            },
        )
        .expect("sound");

    // A grade on one clip and not the others, so the sweeps cover both the
    // flag byte and the thirty-two that follow it, and so a reader that wrote
    // the same answer for every clip would be caught.
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(Digest::of(b"a look")),
            },
        )
        .expect("a grade");

    animate(&mut project, sequence);

    // A second, empty sequence, because one of everything is not a test.
    project.add_sequence(Timebase::PAL_25).expect("room");
    project
}

/// Put a curve on each automation lane.
///
/// Separate from the fixture only because the fixture had grown past what one
/// function should hold. Both lanes are animated, and that is the point: the
/// sweeps below cover bytes that are actually written, so a lane left empty
/// here is a lane the format could forget entirely without a test noticing.
/// It did — the first version of this animated only the picture track, and
/// dropping the level curve from the writer broke nothing at all.
fn animate(project: &mut Project, sequence: sapstudio_model::SequenceId) {
    // An ease rather than a linear on at least one keyframe, because it is the
    // only interpolation that writes anything after its tag — and a format bug
    // in a variable-length record is the kind that reads the next field as
    // part of this one.
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            Instant::new(0, RATE),
                            Rational::new(0, 1).expect("a value"),
                            Interpolation::ease_in_out().expect("an ease"),
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(300, RATE),
                            Rational::new(7, 8).expect("a value"),
                            Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(1_798, RATE),
                            Rational::new(1, 3).expect("a value"),
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("an automation");

    // And a fader curve on the sound track.
    project
        .apply(
            sequence,
            Edit::SetTrackLevel {
                track: 1,
                level: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            Instant::new(0, RATE),
                            Rational::new(-60, 1).expect("a value"),
                            Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(120, RATE),
                            Rational::new(-15, 2).expect("a value"),
                            Interpolation::ease_in_out().expect("an ease"),
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(2_400, RATE),
                            Rational::new(0, 1).expect("a value"),
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("an automation");
}

/// A project that has been through a file, which is what a loaded one is.
fn round_tripped(project: &Project) -> Project {
    decode(&encode(project).expect("an encoding")).expect("a decoding")
}

#[test]
fn a_project_survives_a_round_trip() {
    let original = round_tripped(&sample());
    let again = round_tripped(&original);
    assert_eq!(original, again, "the model is the same after a second trip");
}

#[test]
fn the_encoding_is_canonical() {
    let first = encode(&sample()).expect("an encoding");
    let reloaded = decode(&first).expect("a decoding");
    let second = encode(&reloaded).expect("an encoding");
    assert_eq!(
        first, second,
        "encoding a decoded file must reproduce it byte for byte"
    );
}

#[test]
fn a_curve_survives_the_file_with_every_keyframe_intact() {
    // Round-trip equality would pass for a format that wrote nothing and
    // rebuilt a default, so this looks at the curve itself: the instants, the
    // values as the exact fractions they are, and the interpolation of each
    // keyframe including an ease's four handles.
    let original = sample();
    let loaded = round_tripped(&original);
    let before = original
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an automation")
        .clone();
    let after = loaded
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an automation")
        .clone();
    assert_eq!(before, after);
    assert_eq!(after.keyframes().len(), 3);
    assert_eq!(
        after.keyframes()[2].value(),
        Rational::new(1, 3).expect("a value"),
        "a third is not a decimal and must not have become one"
    );
    assert!(matches!(
        after.keyframes()[0].interpolation(),
        Interpolation::Ease { .. }
    ));

    // And the curve still answers the same, which is what it is for.
    for frame in [0, 100, 300, 1_000, 1_798, 5_000] {
        assert_eq!(
            after.value_at(Instant::new(frame, RATE)),
            before.value_at(Instant::new(frame, RATE)),
            "frame {frame} reads differently after a trip through a file"
        );
    }
}

#[test]
fn a_grade_survives_the_file_and_only_the_clip_that_has_one() {
    // Named separately from the round trip because a reader that gave every
    // clip the same grade, or none, would round-trip perfectly against itself.
    let loaded = round_tripped(&sample());
    let track = loaded
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track");

    let sapstudio_model::Item::Clip(first) = track.item(0).expect("an item") else {
        panic!("not a clip");
    };
    assert_eq!(
        first.grade(),
        Some(Digest::of(b"a look")),
        "the grade did not survive"
    );

    // The gap at index one has none, and the second clip at index two was
    // never given one — so a writer that wrote a grade unconditionally, or a
    // reader that carried the last one forward, is caught here.
    let sapstudio_model::Item::Clip(second) = track.item(2).expect("an item") else {
        panic!("not a clip");
    };
    assert_eq!(
        second.grade(),
        None,
        "a grade appeared on a clip without one"
    );
}

#[test]
fn a_byte_naming_neither_a_grade_nor_its_absence_is_refused() {
    // The flag is nought or one. Anything else is a file this reader does not
    // understand, and reading an unknown value as "no grade" would drop a look
    // while the file still said there was one.
    let file = encode(&sample()).expect("an encoding");
    let mut found = false;
    for index in HEADER_BYTES..file.len() {
        let mut mutated = file.clone();
        mutated[index] = 99;
        if matches!(decode(&mend(&mutated)), Err(IoStatus::UnknownGradeTag(99))) {
            found = true;
            break;
        }
    }
    assert!(found, "no byte in the file is a grade flag");
}

#[test]
fn both_automation_lanes_survive_the_file() {
    // Named separately from the curve round trip because they are separate
    // fields written one behind the other, and a reader that read the same one
    // twice, or read them in the wrong order, would round-trip a project whose
    // picture faded like its sound.
    let original = sample();
    let loaded = round_tripped(&original);
    let sequence = loaded.sequences().iter().next().expect("a sequence").1;

    let opacity = sequence
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an opacity curve");
    let level = sequence
        .track(1)
        .expect("a track")
        .level()
        .expect("a level curve");
    assert_ne!(
        opacity.keyframes()[0].value(),
        level.keyframes()[0].value(),
        "the two lanes came back holding the same thing"
    );
    assert_eq!(
        level.keyframes()[0].value(),
        Rational::new(-60, 1).expect("a value")
    );
    assert_eq!(
        level.keyframes()[1].value(),
        Rational::new(-15, 2).expect("a value"),
        "minus seven and a half decibels is not a whole number and must not \
         have become one"
    );

    // The lanes do not cross: a picture track has no level and a sound track
    // has no opacity, and the model refuses either, so a reader that swapped
    // them would not have got this far.
    assert!(sequence.track(0).expect("a track").level().is_none());
    assert!(sequence.track(1).expect("a track").opacity().is_none());
}

#[test]
fn a_byte_naming_no_interpolation_is_refused() {
    // A tag this build does not know must be refused rather than defaulted:
    // reading an unknown interpolation as a hold would turn somebody's ease
    // into a step, silently, and the file would still say it was an ease.
    let file = encode(&sample()).expect("an encoding");
    let mut found = false;
    for index in HEADER_BYTES..file.len() {
        let mut mutated = file.clone();
        mutated[index] = 99;
        if matches!(
            decode(&mend(&mutated)),
            Err(IoStatus::UnknownInterpolationTag(99))
        ) {
            found = true;
            break;
        }
    }
    assert!(found, "no byte in the file is an interpolation tag");
}

/// Recompute a file's digest, so a deliberate edit reaches the checks past it.
///
/// This format's digest covers the payload alone, unlike the reel's and the
/// summary's, which cover their headers too. That is not an oversight here and
/// the byte sweep is the evidence: every field in this header — the magic, the
/// version, the reserved bytes, the payload length — is checked against
/// something else, so a mutation to any of them is refused on its own terms.
/// A summary's header carries the digest of the sound it summarises, which
/// nothing else can check, and that is why that one is inside its digest.
fn mend(file: &[u8]) -> std::vec::Vec<u8> {
    let mut held = file.to_vec();
    let digest = sapstudio_core::Digest::of(&held[HEADER_BYTES..]);
    held[16..HEADER_BYTES].copy_from_slice(digest.bytes());
    held
}

#[test]
fn the_header_is_what_it_says_it_is() {
    let file = encode(&sample()).expect("an encoding");
    assert!(file.len() > HEADER_BYTES);
    assert_eq!(&file[..4], &MAGIC);
    assert_eq!(u16::from_le_bytes([file[4], file[5]]), FORMAT_VERSION);
    assert_eq!(u16::from_le_bytes([file[6], file[7]]), 0, "reserved");

    let mut length = [0_u8; 8];
    length.copy_from_slice(&file[8..16]);
    let declared = usize::try_from(u64::from_le_bytes(length)).expect("a length that fits");
    assert_eq!(declared, file.len() - HEADER_BYTES);

    let mut digest = [0_u8; 32];
    digest.copy_from_slice(&file[16..48]);
    assert_eq!(
        Digest::new(digest),
        Digest::of(&file[HEADER_BYTES..]),
        "the header's digest is the payload's"
    );
}

#[test]
fn history_is_not_saved() {
    let mut project = sample();
    assert!(project.history().undo_depth() > 0, "the sample was edited");
    project = round_tripped(&project);
    assert_eq!(
        project.history().undo_depth(),
        0,
        "a freshly opened project has nothing to undo"
    );
    assert_eq!(project.history().redo_depth(), 0);
}

#[test]
fn a_file_that_is_not_one_is_refused() {
    assert_eq!(decode(b""), Err(IoStatus::TruncatedHeader));
    assert_eq!(decode(&[0; 47]), Err(IoStatus::TruncatedHeader));

    let mut file = encode(&sample()).expect("an encoding");
    file[0] = b'X';
    assert_eq!(decode(&file), Err(IoStatus::NotAProjectFile));
}

#[test]
fn a_future_version_is_refused_by_number() {
    let mut file = encode(&sample()).expect("an encoding");
    file[4..6].copy_from_slice(&(FORMAT_VERSION + 1).to_le_bytes());
    assert_eq!(
        decode(&file),
        Err(IoStatus::UnsupportedVersion(FORMAT_VERSION + 1)),
        "a reader that guessed at a later format would be guessing at the user's work"
    );
}

#[test]
fn a_set_reserved_field_is_refused() {
    let mut file = encode(&sample()).expect("an encoding");
    file[6] = 1;
    assert_eq!(decode(&file), Err(IoStatus::ReservedFieldSet));
}

#[test]
fn a_wrong_length_is_refused_either_way() {
    let file = encode(&sample()).expect("an encoding");

    let mut short = file.clone();
    let declared = (file.len() - HEADER_BYTES) as u64;
    short[8..16].copy_from_slice(&(declared + 1).to_le_bytes());
    assert_eq!(decode(&short), Err(IoStatus::TruncatedPayload));

    let mut long = file.clone();
    long[8..16].copy_from_slice(&(declared - 1).to_le_bytes());
    assert_eq!(decode(&long), Err(IoStatus::TrailingBytes));
}

#[test]
fn a_damaged_payload_is_refused_rather_than_loaded() {
    let mut file = encode(&sample()).expect("an encoding");
    let middle = HEADER_BYTES + (file.len() - HEADER_BYTES) / 2;
    file[middle] ^= 0x01;
    assert_eq!(
        decode(&file),
        Err(IoStatus::DigestMismatch),
        "one flipped bit anywhere in a saved project is one flipped bit too many"
    );
}

#[test]
fn every_single_byte_change_is_refused() {
    // The strongest claim this format makes: there is no byte of a valid file
    // that can be changed to anything else and still be read. The digest
    // covers the payload, the header covers the digest, and the magic and
    // version cover the header.
    let file = encode(&sample()).expect("an encoding");
    let mut checked = 0_usize;
    for index in 0..file.len() {
        for replacement in [0x00_u8, 0x01, 0x7F, 0x80, 0xFF] {
            if file[index] == replacement {
                continue;
            }
            let mut mutated = file.clone();
            mutated[index] = replacement;
            assert!(
                decode(&mutated).is_err(),
                "byte {index} changed to {replacement:#04x} was accepted"
            );
            checked += 1;
        }
    }
    assert!(checked > 1000, "the sweep covered only {checked} mutations");
}

#[test]
fn every_prefix_of_a_file_is_refused() {
    let file = encode(&sample()).expect("an encoding");
    for length in 0..file.len() {
        assert!(
            decode(&file[..length]).is_err(),
            "a file truncated to {length} bytes was accepted"
        );
    }
    assert!(decode(&file).is_ok(), "the whole file is still fine");
}

#[test]
fn every_extension_of_a_file_is_refused() {
    let file = encode(&sample()).expect("an encoding");
    for extra in 1..64_usize {
        let mut extended = file.clone();
        extended.resize(file.len() + extra, 0);
        assert_eq!(decode(&extended), Err(IoStatus::TrailingBytes));
    }
}

/// xorshift64*, so a failure is a seed rather than an anecdote.
struct Generator(u64);

impl Generator {
    fn next(&mut self) -> u64 {
        let mut state = self.0;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        self.0 = state;
        state.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
}

#[test]
fn garbage_is_refused_and_never_anything_worse() {
    // A project file arrives from outside and is hostile until proven
    // otherwise (R-11). The decoder may refuse it; it may not loop, exhaust
    // memory, or panic.
    let mut generator = Generator(0x0BAD_C0DE_D15E_A5E1);
    for _ in 0..2000 {
        let length = usize::try_from(generator.next() % 512).unwrap_or(0);
        let mut bytes: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            bytes.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        assert!(decode(&bytes).is_err(), "random bytes were accepted");
    }
}

#[test]
fn garbage_behind_a_valid_header_is_refused() {
    // The harder case: the magic, version, length, and digest all agree, and
    // the payload is nonsense. Now every refusal has to come from the
    // structure itself.
    let mut generator = Generator(0x5EED_1234_5678_9ABC);
    let mut refusals = 0_usize;
    for _ in 0..4000 {
        let length = usize::try_from(generator.next() % 256).unwrap_or(0);
        let mut payload: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            payload.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        let mut file: std::vec::Vec<u8> = std::vec::Vec::new();
        file.extend_from_slice(&MAGIC);
        file.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
        file.extend_from_slice(&0_u16.to_le_bytes());
        file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
        file.extend_from_slice(Digest::of(&payload).bytes());
        file.extend_from_slice(&payload);

        // A well-formed empty payload is a real, empty project; anything else
        // must be refused by the structure.
        match decode(&file) {
            Ok(project) => assert!(
                project.media().is_empty() && project.sequences().is_empty(),
                "a nonsense payload decoded into a non-empty project"
            ),
            Err(_) => refusals += 1,
        }
    }
    assert!(
        refusals > 3000,
        "only {refusals} of 4000 payloads were refused"
    );
}

#[test]
fn a_clip_naming_media_the_file_does_not_have_is_refused() {
    // Hand-built: no media, one sequence, one track, one clip pointing at
    // media index zero.
    let mut payload: std::vec::Vec<u8> = std::vec::Vec::new();
    payload.extend_from_slice(&0_u32.to_le_bytes()); // no media
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one sequence
    payload.extend_from_slice(&30_000_i64.to_le_bytes());
    payload.extend_from_slice(&1_001_i64.to_le_bytes());
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one track
    payload.push(0); // video
    payload.push(1); // a fader at a level
    payload.extend_from_slice(&0_i64.to_le_bytes()); // zero decibels
    payload.extend_from_slice(&1_i64.to_le_bytes());
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one item
    payload.push(0); // a clip
    payload.extend_from_slice(&0_u32.to_le_bytes()); // media index zero
    payload.extend_from_slice(&0_i64.to_le_bytes());
    payload.extend_from_slice(&100_i64.to_le_bytes());

    let mut file: std::vec::Vec<u8> = std::vec::Vec::new();
    file.extend_from_slice(&MAGIC);
    file.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
    file.extend_from_slice(&0_u16.to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(Digest::of(&payload).bytes());
    file.extend_from_slice(&payload);

    assert_eq!(decode(&file), Err(IoStatus::MediaIndexOutOfRange));
}

#[test]
fn a_faders_position_survives_the_file() {
    // A mix that did not survive a save would be a mix nobody could deliver.
    // Every kind of position is checked, including the two that are easy to
    // conflate: a track at the bottom of its travel, and a track that is off.
    use sapstudio_core::Rational;
    use sapstudio_model::{Fader, MINIMUM_DECIBELS};

    let positions = [
        Fader::UNITY,
        Fader::MUTED,
        Fader::at(Rational::new(MINIMUM_DECIBELS, 1).expect("a ratio")).expect("a level"),
        Fader::at(Rational::new(-15, 2).expect("a ratio")).expect("a level"),
        Fader::at(Rational::new(24, 1).expect("a ratio")).expect("a level"),
    ];

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    for (index, fader) in positions.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: TrackKind::Audio,
                },
            )
            .expect("a track");
        project
            .apply(
                sequence,
                Edit::SetTrackFader {
                    track: index,
                    fader: *fader,
                },
            )
            .expect("a fader move");
    }

    let file = encode(&project).expect("an encoding");
    let back = decode(&file).expect("a decoding");
    for (index, fader) in positions.iter().enumerate() {
        assert_eq!(
            back.sequence(sequence)
                .expect("a sequence")
                .track(index)
                .expect("a track")
                .fader(),
            *fader,
            "track {index}"
        );
    }

    // A quarter of a decibel is not a round number in any binary, which is why
    // the fader is stored as the exact ratio the user set rather than as a
    // factor: it comes back as the same ratio, not as one near it.
    assert_eq!(
        back.sequence(sequence)
            .expect("a sequence")
            .track(3)
            .expect("a track")
            .fader()
            .decibels(),
        Some(Rational::new(-15, 2).expect("a ratio"))
    );
}

#[test]
fn a_version_one_project_is_refused_rather_than_read_as_version_two() {
    // Version one had no fader, so a version-one file read as version two
    // would take its faders out of whatever followed the track kind — which is
    // an item count. The version number is what stops that, and it is checked
    // before any field is.
    let project = sample();
    let mut file = encode(&project).expect("an encoding");
    file[4] = 1;
    file[5] = 0;
    assert_eq!(decode(&file), Err(IoStatus::UnsupportedVersion(1)));
}

#[test]
fn a_fader_tag_this_build_does_not_know_is_refused() {
    use sapstudio_core::Rational;
    use sapstudio_model::Fader;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: Fader::at(Rational::new(-6, 1).expect("a ratio")).expect("a level"),
            },
        )
        .expect("a fader move");

    let file = encode(&project).expect("an encoding");
    // The digest covers the payload, so a mutated file is caught by that
    // first. This finds the tag byte by rebuilding the file around a bad one
    // instead, so it is the tag reader that refuses rather than the digest.
    let mut forged = file.clone();
    // The fader tag is the byte 1 that precedes the exact ratio -6/1, which
    // nothing else in this file can be.
    let needle = {
        let mut bytes = std::vec::Vec::new();
        bytes.push(1_u8);
        bytes.extend_from_slice(&(-6_i64).to_le_bytes());
        bytes.extend_from_slice(&1_i64.to_le_bytes());
        bytes
    };
    let at = forged
        .windows(needle.len())
        .position(|window| window == needle.as_slice())
        .expect("the fader is in the file");
    forged[at] = 9;
    let payload = forged[HEADER_BYTES..].to_vec();
    let digest = Digest::of(&payload);
    forged[HEADER_BYTES - 32..HEADER_BYTES].copy_from_slice(digest.bytes());

    assert_eq!(decode(&forged), Err(IoStatus::UnknownFaderTag(9)));
}

#[test]
fn a_dissolve_survives_the_file() {
    use sapstudio_model::Transition;

    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::of(b"a"), RATE, frames(10_000)).expect("an asset");
    let id = project.add_media(asset).expect("an identifier");
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for (index, source_start) in [(0, 0_i64), (1, 100), (2, 200)] {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(id, source_start, frames(50)).expect("a clip")),
                },
            )
            .expect("an insert");
    }
    // Two dissolves, on the two cuts, of different lengths — so a reader that
    // wrote one length for both, or lost the order, would be caught.
    let transitions = [
        Transition::new(1, frames(12)).expect("a dissolve"),
        Transition::new(2, frames(30)).expect("a dissolve"),
    ];
    for transition in transitions {
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition,
                },
            )
            .expect("a dissolve");
    }

    let back = round_tripped(&project);
    let track = back
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track");
    assert_eq!(track.transitions(), &transitions);
}

#[test]
fn a_file_whose_dissolve_the_model_would_refuse_is_refused() {
    use sapstudio_model::Transition;

    // A well-formed file, then the same file with its dissolve moved onto a
    // cut that has a gap on one side. The digest is recomputed so that it is
    // the model's refusal doing the work rather than the integrity check.
    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::of(b"a"), RATE, frames(10_000)).expect("an asset");
    let id = project.add_media(asset).expect("an identifier");
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for (index, item) in [
        Item::Clip(Clip::new(id, 0, frames(50)).expect("a clip")),
        Item::Clip(Clip::new(id, 100, frames(50)).expect("a clip")),
        Item::gap(frames(50)).expect("a gap"),
    ]
    .into_iter()
    .enumerate()
    {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item,
                },
            )
            .expect("an insert");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let file = encode(&project).expect("an encoding");
    assert!(decode(&file).is_ok());

    // The boundary is written as a u32 immediately before the duration, so
    // finding the pair identifies it without guessing at offsets.
    let needle = {
        let mut bytes = std::vec::Vec::new();
        bytes.extend_from_slice(&1_u32.to_le_bytes());
        bytes.extend_from_slice(&12_i64.to_le_bytes());
        bytes
    };
    let mut forged = file.clone();
    let at = forged
        .windows(needle.len())
        .position(|window| window == needle.as_slice())
        .expect("the dissolve is in the file");
    forged[at..at + 4].copy_from_slice(&2_u32.to_le_bytes());
    let payload = forged[HEADER_BYTES..].to_vec();
    forged[HEADER_BYTES - 32..HEADER_BYTES].copy_from_slice(Digest::of(&payload).bytes());

    assert_eq!(
        decode(&forged),
        Err(IoStatus::Model(sapstudio_model::ModelStatus::NotAClip)),
        "a dissolve onto a gap is refused on the way in, not stored and \
         discovered later"
    );
}

#[test]
fn a_wipe_survives_the_file_with_its_direction() {
    // Version seven's whole reason. A transition used to be a boundary and a
    // length, and reading one back could only produce a dissolve because that
    // was the only kind there was. The tag says which, and the direction is
    // written as two rationals rather than an angle, so a wipe drawn at a
    // third of a turn is the same wipe when it comes back rather than a
    // rounding of one.
    use sapstudio_model::{Transition, TransitionKind, Wipe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"wiped"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    let wipe = Wipe::new(
        Rational::new(3, 7).expect("a rational"),
        Rational::new(-2, 5).expect("a rational"),
    )
    .expect("a wipe");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(12), wipe).expect("a wipe"),
            },
        )
        .expect("a wipe");

    let back = round_tripped(&project);
    let held = back
        .sequence(back.sequences().iter().next().expect("a sequence").0)
        .expect("a sequence");
    let transition = held.track(0).expect("a track").transitions()[0];
    assert_eq!(transition.kind(), TransitionKind::Wipe(wipe));
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "and the whole sequence comes back equal, not just the direction"
    );
}

#[test]
fn a_transition_tag_this_build_does_not_read_is_refused() {
    use sapstudio_model::Transition;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"tagged"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    // The tag check cannot be reached by mutating a byte: the digest refuses
    // first, which is exactly what the byte sweep in this file asserts. So the
    // file is *resealed* around the changed byte -- the payload's digest
    // recomputed and written back into the header -- which is what a reader
    // has to survive being handed by something that meant it.
    let file = encode(&project).expect("an encoding");
    let mut found = None;
    for index in HEADER_BYTES..file.len() {
        if file[index] != 0 {
            continue;
        }
        let mut broken = file.clone();
        broken[index] = 2;
        let sealed = Digest::of(&broken[HEADER_BYTES..]);
        broken[16..48].copy_from_slice(sealed.bytes());
        if decode(&broken) == Err(IoStatus::UnknownTransitionTag(2)) {
            found = Some(index);
            break;
        }
    }
    assert!(
        found.is_some(),
        "the transition kind has to be a tag some byte carries, or it is not a tag"
    );
}

#[test]
fn a_wipes_softness_survives_the_file() {
    use sapstudio_model::{Transition, TransitionKind, Wipe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"soft"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    // A third is the value that catches a format storing softness as anything
    // binary: it is exact as a rational and is not exact as any fixed-point
    // number this project could have reached for.
    let wipe = Wipe::soft(
        Rational::ONE,
        Rational::ZERO,
        Rational::new(1, 3).expect("a third"),
    )
    .expect("a wipe");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(12), wipe).expect("a wipe"),
            },
        )
        .expect("a wipe");

    let back = round_tripped(&project);
    let held = back
        .sequence(back.sequences().iter().next().expect("a sequence").0)
        .expect("a sequence");
    let TransitionKind::Wipe(read) = held.track(0).expect("a track").transitions()[0].kind() else {
        panic!("a wipe went in and something else came out");
    };
    assert_eq!(read.softness(), Rational::new(1, 3).expect("a third"));
    assert_eq!(read, wipe);
}

#[test]
fn a_mask_survives_the_file_with_its_corners_and_its_inversion() {
    use sapstudio_model::Mask;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"masked"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    // Sevenths and elevenths: exact as rationals, and not exact as anything
    // binary. A mask stored as fixed-point would come back a different shape.
    let mask = Mask::new(vec![
        (
            Rational::new(1, 7).expect("a rational"),
            Rational::new(2, 11).expect("a rational"),
        ),
        (
            Rational::new(6, 7).expect("a rational"),
            Rational::new(3, 11).expect("a rational"),
        ),
        (
            Rational::new(5, 7).expect("a rational"),
            Rational::new(9, 11).expect("a rational"),
        ),
    ])
    .expect("a triangle")
    .inverted();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask.clone()),
            },
        )
        .expect("a mask");

    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "the whole sequence comes back equal, corners and inversion included"
    );
}

#[test]
fn a_file_holding_a_concave_mask_is_refused() {
    // The model refuses a concave outline, and the file goes through the same
    // constructor -- so a project cannot hold a shape by being written down
    // that it could not hold by being edited.
    //
    // Reaching that check needs a *resealed* file, because the digest refuses
    // a mutated byte long before any field is parsed. And it needs the right
    // bytes: the corner is written as two rationals, each a numerator and a
    // denominator, so the third corner of an eight-by-eight square is the
    // eight consecutive little-endian words `8, 1, 8, 1`. Pulling that corner
    // in to (3, 3) turns the square into an arrowhead.
    use sapstudio_model::Mask;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"bent"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::from_integer(8),
        Rational::from_integer(8),
    )
    .expect("a square");
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask),
            },
        )
        .expect("a mask");

    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [8_i64, 1, 8, 1] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the third corner is in the file");

    let mut bent = file.clone();
    for offset in [0_usize, 16] {
        bent[at + offset..at + offset + 8].copy_from_slice(&3_i64.to_le_bytes());
    }
    let sealed = Digest::of(&bent[HEADER_BYTES..]);
    bent[16..48].copy_from_slice(sealed.bytes());

    assert_eq!(
        decode(&bent),
        Err(IoStatus::Model(sapstudio_model::ModelStatus::MaskNotConvex)),
        "a file describing a shape the model would refuse has to be refused too"
    );
}

#[test]
fn a_location_hint_survives_the_file_uninterpreted() {
    use sapstudio_model::Location;

    let mut project = Project::new();
    let id = project
        .add_media(MediaAsset::new(Digest::of(b"placed"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    // Not valid text, on purpose: a path is whatever the platform says it is,
    // and a format that round-trips only the paths it can read is a format
    // that loses somebody's media.
    let hint = Location::new(&[0xFF, b'/', 0x00, 0x80, b'r']).expect("a hint");
    project
        .set_media_location(id, Some(hint.clone()))
        .expect("an asset");

    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    assert_eq!(asset.location(), Some(&hint));
}

#[test]
fn an_asset_with_no_hint_costs_four_bytes_and_comes_back_with_none() {
    let mut project = Project::new();
    project
        .add_media(MediaAsset::new(Digest::of(b"bare"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    assert_eq!(asset.location(), None, "absent is absent, not empty");
}

#[test]
fn a_file_listing_one_piece_of_content_twice_is_refused() {
    // `add_media` hands back the identifier it already had, which is right for
    // the API and wrong for a file: every clip indexing the second record
    // would then point at the first, which is a different programme arrived at
    // silently. Reaching the check needs a resealed file, because the digest
    // refuses a mutated byte first.
    let mut project = Project::new();
    for tag in [b"one".as_slice(), b"two".as_slice()] {
        project
            .add_media(MediaAsset::new(Digest::of(tag), RATE, frames(9_000)).expect("an asset"))
            .expect("room");
    }
    let file = encode(&project).expect("an encoding");

    // The two records are the first thing in the payload after the count, and
    // each begins with its digest. Making the second digest equal the first is
    // what a file that lists one asset twice looks like.
    let first: Vec<u8> = file[HEADER_BYTES + 4..HEADER_BYTES + 36].to_vec();
    let second = file
        .windows(32)
        .position(|window| window == Digest::of(b"two".as_slice()).bytes().as_slice())
        .expect("the second digest is in the file");
    let mut doubled = file.clone();
    doubled[second..second + 32].copy_from_slice(&first);
    let sealed = Digest::of(&doubled[HEADER_BYTES..]);
    doubled[16..48].copy_from_slice(sealed.bytes());

    assert_eq!(decode(&doubled), Err(IoStatus::DuplicateMedia));
}
