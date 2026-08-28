// SPDX-License-Identifier: GPL-3.0-only
//! The point a framing acts about.
//!
//! M8.9 fixed it at the frame's centre with a good argument: scaling about the
//! corner "sends the picture sliding off to the lower right the moment
//! somebody drags a scale slider", which is not what anybody means by "make it
//! bigger". The centre is the right *default* and it is a poor *rule* — a
//! lower third that swings in pivots on its left edge, and a card that flips
//! pivots on the line it flips about.
//!
//! M8.24 made the case sharper than it was, which is why this arrives now
//! rather than three milestones ago: a turn about the centre is a turn about
//! the centre, and every other pivot was unreachable.

use sapstudio_core::{Digest, Duration, Instant, Rational, Timebase};
use sapstudio_model::{
    Clip, Curve, Edit, Interpolation, Item, Keyframe, Lane, MediaAsset, Motion, Project,
    Resampling, SequenceId, TrackKind, Transform, Turn,
};

const RATE: Timebase = Timebase::FILM_24;
const LENGTH: i64 = 24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

fn framing() -> Transform {
    Transform::scaled(
        Rational::ONE,
        Rational::ONE,
        (Rational::ZERO, Rational::ZERO),
        Resampling::Bilinear,
    )
    .expect("a transform")
}

#[test]
fn a_transform_pivots_on_the_centre_until_somebody_moves_it() {
    // The default is the decision M8.9 made, kept. What is new is that it is a
    // default rather than the only answer.
    assert_eq!(framing().anchor(), (r(1, 2), r(1, 2)));
    assert_eq!(
        framing()
            .with_anchor((Rational::ZERO, Rational::ZERO))
            .anchor(),
        (Rational::ZERO, Rational::ZERO)
    );
}

#[test]
fn a_pivot_outside_the_frame_is_a_pivot() {
    // No refusal, and that is a decision rather than an omission: a card
    // swinging in from above the picture turns about a point above it, so a
    // bound here would be a bound on moves rather than on values.
    for anchor in [
        (r(-3, 1), r(1, 2)),
        (r(1, 2), r(7, 2)),
        (r(-1, 8), r(-1, 8)),
    ] {
        assert_eq!(framing().with_anchor(anchor).anchor(), anchor);
    }
}

#[test]
fn the_pivot_is_not_part_of_being_still() {
    // The identity fixes every point, so it fixes the anchor too. Consulting
    // the anchor here would send every clip that has one through a resampler
    // to compute the picture it already had.
    assert!(framing().is_still());
    assert!(
        framing().with_anchor((Rational::ZERO, r(3, 4))).is_still(),
        "a transform that moves nothing moves nothing about any point"
    );
    // And a transform that does move is not still whatever its pivot is.
    let scaled = Transform::scaled(
        r(2, 1),
        r(2, 1),
        (Rational::ZERO, Rational::ZERO),
        Resampling::Bilinear,
    )
    .expect("a transform");
    assert!(!scaled.is_still());
    assert!(
        !scaled
            .with_anchor((Rational::ZERO, Rational::ZERO))
            .is_still()
    );
}

#[test]
fn an_animated_clip_keeps_the_pivot_its_framing_was_given() {
    // `Transform::new` starts from nothing, which is right in a constructor and
    // wrong in `moved_by` -- rebuilding through it would move every animated
    // clip's pivot quietly back to the centre. That is the third field to find
    // this trap, after the grade and the motion, and the fix is the same one:
    // change one field rather than rebuild from several.
    let corner = (Rational::ZERO, Rational::ZERO);
    let based = framing().with_anchor(corner);
    let moved = based
        .moved_by(r(3, 2), r(1, 10), r(-1, 10), Turn::NONE)
        .expect("a framing");
    assert_eq!(
        moved.anchor(),
        corner,
        "the animation moved the pivot back to the centre"
    );
    // And through the layer stack, which is where an editor would meet it.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(based),
            },
        )
        .expect("a framing");
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(
                    Motion::new(
                        Some(
                            Curve::new(std::vec![
                                Keyframe::new(at(0), Rational::ONE, Interpolation::Linear)
                                    .expect("a keyframe"),
                                Keyframe::new(at(LENGTH), r(2, 1), Interpolation::Linear)
                                    .expect("a keyframe"),
                            ])
                            .expect("a curve"),
                        ),
                        None,
                        None,
                        None,
                    )
                    .expect("a motion"),
                ),
            },
        )
        .expect("a motion");
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(LENGTH / 2))
        .expect("a stack");
    assert_eq!(
        stack[0].transform().expect("a framing").anchor(),
        corner,
        "the layer lost the pivot half way through the push-in"
    );
}

fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
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
                item: Item::Clip(Clip::new(media, 100, frames(LENGTH)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}
