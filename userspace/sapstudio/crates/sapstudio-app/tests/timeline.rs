// SPDX-License-Identifier: GPL-3.0-only
//! A sequence, rendered: the timeline and the compositor meeting.

use sapstudio_app::SlateStatus;
use sapstudio_app::timeline;
use sapstudio_core::Digest;
use sapstudio_core::{Duration, Instant, Timebase};
use sapstudio_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat,
};
use sapstudio_model::{Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind};
use sapstudio_render::{Library, Look, RenderStatus};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

/// The description everything in this file is rendered in.
fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// A frame source that hands back flat colours, keyed by media.
///
/// Premultiplied by construction: the colour never exceeds the coverage, so
/// every frame it produces is one the compositor will accept.
struct Flat {
    colours: std::vec::Vec<(Digest, [u8; 4])>,
    description: FrameDescription,
    asked: std::vec::Vec<(Digest, i64)>,
    looks: std::vec::Vec<(Digest, Look)>,
    /// Answer with `description` rather than with what was asked for.
    ///
    /// A field rather than the default behaviour, which it used to be. A
    /// source that ignores the description it is given is a *fault*, and one
    /// test exists to prove the graph refuses it — but every other test then
    /// depended on the fault by accident, and a graded layer, which is fetched
    /// straight, could not render at all. The lie is deliberate now and only
    /// where it is the subject.
    answers_wrongly: bool,
}

impl Library for Flat {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.asked.push((media, tick));
        let colour = self
            .colours
            .iter()
            .find(|(id, _)| *id == media)
            .map_or([0, 0, 0, 255], |(_, colour)| *colour);
        // Answer the description that was asked for, unless this fixture is
        // deliberately being the source that does not. It used to always
        // ignore it, which was harmless only while every layer was fetched the
        // same way — a graded layer is fetched straight, and a source that
        // hands back something else has answered a different question.
        let mut bytes = std::vec::Vec::new();
        for _ in 0..16 {
            bytes.extend_from_slice(&colour);
        }
        let answer = if self.answers_wrongly {
            self.description
        } else {
            description
        };
        Frame::from_packed(answer, &bytes).map_err(RenderStatus::Media)
    }

    fn look(&mut self, look: sapstudio_core::Digest) -> Result<Look, RenderStatus> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(RenderStatus::UnknownNode)
    }
}

/// The content digest of a media asset, which is how the graph names it.
fn digest_of(project: &Project, id: MediaId) -> Digest {
    project.media().get(id).expect("an asset").digest()
}

/// A fresh pool for each render.
///
/// Deliberately not shared: most of these tests are about what a render
/// produces, and a pool that outlived one call would let a stale frame answer
/// for a changed graph. The caching test builds its own and keeps it.
fn pool() -> FramePool {
    FramePool::new(64, 1 << 20)
}

/// One pixel of a rendered frame.
fn pixel(frame: &Frame) -> (u8, u8, u8, u8) {
    let bytes = frame.to_packed().expect("bytes");
    (bytes[0], bytes[1], bytes[2], bytes[3])
}

fn lay(project: &mut Project, sequence: SequenceId, track: usize, items: &[Item]) {
    while project
        .sequence(sequence)
        .expect("a sequence")
        .track_count()
        <= track
    {
        let index = project
            .sequence(sequence)
            .expect("a sequence")
            .track_count();
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: TrackKind::Video,
                },
            )
            .expect("a track");
    }
    for (index, item) in items.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track,
                    index,
                    item: item.clone(),
                },
            )
            .expect("an insert");
    }
}

fn media(project: &mut Project, tag: u8) -> MediaId {
    let mut bytes = [0_u8; 32];
    bytes[0] = tag;
    let asset = MediaAsset::new(
        sapstudio_model::media::Digest::new(bytes),
        RATE,
        frames(1000),
    )
    .expect("an asset");
    project.add_media(asset).expect("an identifier")
}

#[test]
fn an_empty_sequence_shows_opaque_black() {
    // Not a hole. A viewer shows black leader and an export writes black
    // frames; neither shows whatever was behind the window. So the programme
    // is opaque even where nothing is on it.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (0, 0, 0, 255));
    assert!(source.asked.is_empty(), "nothing was decoded for nothing");
}

#[test]
fn one_opaque_layer_is_what_you_see() {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let red = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(red, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, red), [200, 30, 40, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (200, 30, 40, 255));
    assert_eq!(
        source.asked,
        std::vec![(digest_of(&project, red), 5)],
        "one frame, the right one"
    );
}

#[test]
fn an_upper_track_covers_a_lower_one_and_a_gap_lets_it_through() {
    // The two halves of the same decision, in the same fixture: V2 covers V1
    // where it has material, and shows it where it has a gap. A gap that
    // contributed black would make the first half pass and the second fail.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(20)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[
            Item::gap(frames(10)).expect("a gap"),
            Item::Clip(Clip::new(over, 0, frames(10)).expect("a clip")),
        ],
    );
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 30, 255]),
            (digest_of(&project, over), [90, 80, 70, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };

    let inside_the_gap = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&inside_the_gap),
        (10, 20, 30, 255),
        "V1 shows through V2's gap"
    );

    let covered = timeline::render(
        &project,
        sequence,
        at(15),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&covered),
        (90, 80, 70, 255),
        "and V2 covers it where it has material"
    );
}

#[test]
fn a_half_covered_upper_track_shows_the_lower_one_through_it() {
    // The whole point of doing this with `over` rather than with a copy. A
    // half-covered layer must let the one beneath show through it, in linear
    // light, so a dissolve or a soft-edged title lands without a fringe.
    //
    // The numbers: coverage 128 of white over opaque black is code value 205,
    // which is the pixel the compositor's own tests compute by hand. Here it
    // arrives through the timeline instead, which is the point — the same
    // arithmetic, reached the way a session reaches it.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(over, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        // 188 at coverage 128 is premultiplied white: the compositor refuses
        // anything brighter, which is what keeps this honest.
        colours: std::vec![
            (digest_of(&project, under), [128, 128, 128, 255]),
            (digest_of(&project, over), [188, 188, 188, 128])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&rendered),
        (205, 205, 205, 255),
        "half-covered white over mid-grey, in light"
    );
}

#[test]
fn three_layers_stack_bottom_first() {
    // Order matters and the stack has to get it right, so this uses three
    // opaque layers where only the top one can win.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    let three = media(&mut project, 3);
    for (track, id) in [(0, one), (1, two), (2, three)] {
        lay(
            &mut project,
            sequence,
            track,
            &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
        );
    }
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [1, 1, 1, 255]),
            (digest_of(&project, two), [2, 2, 2, 255]),
            (digest_of(&project, three), [3, 3, 3, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (3, 3, 3, 255), "V3 is on top");
    assert_eq!(
        source.asked,
        std::vec![
            (digest_of(&project, one), 0),
            (digest_of(&project, two), 0),
            (digest_of(&project, three), 0)
        ],
        "and they were fetched bottom first"
    );
}

#[test]
fn the_playhead_asks_for_the_right_source_frame() {
    // The arithmetic a whole clip depends on, checked through the render path
    // rather than only in the model.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::gap(frames(4)).expect("a gap"),
            Item::Clip(Clip::new(id, 100, frames(6)).expect("a clip")),
        ],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [7, 7, 7, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for frame in 0..12 {
        timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
    }
    assert_eq!(
        source.asked,
        std::vec![
            (digest_of(&project, id), 100),
            (digest_of(&project, id), 101),
            (digest_of(&project, id), 102),
            (digest_of(&project, id), 103),
            (digest_of(&project, id), 104),
            (digest_of(&project, id), 105)
        ],
        "six frames, starting at source 100, and nothing outside the clip"
    );
}

#[test]
fn a_source_that_answers_with_the_wrong_frame_is_refused() {
    // Converting it here would be a decision made in the wrong place: the
    // source was told what to produce, and quietly fixing its answer is how a
    // pipeline ends up with a conversion nobody chose. The refusal comes from
    // the graph now rather than from this layer, because that is where the
    // node that asked for the frame lives — `Convert` is a node, with a name.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let wrong = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [10, 10, 10, 255])],
        description: wrong,
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: true,
    };
    assert_eq!(
        timeline::render(
            &project,
            sequence,
            at(0),
            described(),
            &mut pool(),
            &mut source
        ),
        Err(SlateStatus::Render(RenderStatus::SourceDescriptionMismatch))
    );
}

#[test]
fn a_base_that_is_not_premultiplied_is_refused() {
    // `over` is only correct on premultiplied values, so the bottom of the
    // stack has to be one. Refusing rather than converting keeps the decision
    // with the caller who chose the description.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let straight = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: straight,
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    assert_eq!(
        timeline::render(
            &project,
            sequence,
            at(0),
            straight,
            &mut pool(),
            &mut source
        ),
        Err(SlateStatus::BaseNotPremultiplied)
    );
}

#[test]
fn rendering_one_instant_twice_gives_one_answer() {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(one, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(two, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [40, 50, 60, 255]),
            (digest_of(&project, two), [90, 40, 10, 100])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let first = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let second = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(first.digest(), second.digest());
}

#[test]
fn a_dissolve_cross_fades_in_linear_light() {
    // The whole point of representing a dissolve as an opacity: `over` already
    // computes `in x t + out x (1 - t)`, so a cross-fade needs no second
    // operator — and it happens in light, like everything else.
    //
    // White dissolving to black over four frames. Every number below is worked
    // out from the definitions rather than read off a run:
    //
    //   the incoming clip's coverage at n/5 is round(255 x n/5),
    //   the result's light is dec(255) x (1 - coverage/255) = 1 - n/5,
    //   and the sRGB code nearest that light is the answer.
    //
    //   1/5 -> alpha  51 -> light 0.8 -> 231
    //   2/5 -> alpha 102 -> light 0.6 -> 203
    //   3/5 -> alpha 153 -> light 0.4 -> 170
    //   4/5 -> alpha 204 -> light 0.2 -> 124
    //
    // A dissolve done in code values instead would step 255, 204, 153, 102,
    // 51, 0 — evenly spaced numbers, and a visibly wrong fade that goes dark
    // too fast in the middle. That is what these four values catch.
    //
    // What they do *not* catch is scaling a layer's coverage without scaling
    // its colour, because the incoming clip here is black and black has no
    // colour to scale. `a_dissolve_is_opaque_all_the_way_through` uses
    // coloured clips and catches exactly that — a control confirmed the
    // division of labour, so neither test is redundant.
    use sapstudio_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    let black = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(white, 0, frames(20)).expect("a clip")),
            Item::Clip(Clip::new(black, 100, frames(20)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, white), [255, 255, 255, 255]),
            (digest_of(&project, black), [0, 0, 0, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };

    assert_eq!(
        pixel(
            &timeline::render(
                &project,
                sequence,
                at(17),
                described(),
                &mut pool(),
                &mut source
            )
            .expect("a render")
        ),
        (255, 255, 255, 255),
        "the frame before the dissolve is all outgoing"
    );
    for (frame, expected) in [(18, 231_u8), (19, 203), (20, 170), (21, 124)] {
        let rendered = timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        assert_eq!(
            pixel(&rendered),
            (expected, expected, expected, 255),
            "frame {frame} of the dissolve"
        );
    }
    assert_eq!(
        pixel(
            &timeline::render(
                &project,
                sequence,
                at(22),
                described(),
                &mut pool(),
                &mut source
            )
            .expect("a render")
        ),
        (0, 0, 0, 255),
        "and the frame after it is all incoming"
    );
}

#[test]
fn a_dissolve_is_opaque_all_the_way_through() {
    // A cross-fade between two opaque clips must not show the background at
    // any point in it. If the two opacities did not sum to one, the middle of
    // every dissolve would be see-through — which over black looks like a dip
    // to dark, and is the classic wrong dissolve.
    use sapstudio_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(one, 0, frames(20)).expect("a clip")),
            Item::Clip(Clip::new(two, 100, frames(20)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [200, 100, 50, 255]),
            (digest_of(&project, two), [50, 100, 200, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for frame in 14..26 {
        let rendered = timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        assert_eq!(pixel(&rendered).3, 255, "frame {frame} is see-through");
    }
}

#[test]
fn a_dissolve_moves_in_one_direction_all_the_way_across() {
    // No dip, no plateau, no reversal. A fade that went back on itself at any
    // frame would be visible and nobody could say why.
    use sapstudio_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let bright = media(&mut project, 1);
    let dark = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(bright, 0, frames(30)).expect("a clip")),
            Item::Clip(Clip::new(dark, 100, frames(30)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(20)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, bright), [255, 255, 255, 255]),
            (digest_of(&project, dark), [0, 0, 0, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut previous = 256_i32;
    for frame in 20..40 {
        let value = i32::from(
            pixel(
                &timeline::render(
                    &project,
                    sequence,
                    at(frame),
                    described(),
                    &mut pool(),
                    &mut source,
                )
                .expect("a render"),
            )
            .0,
        );
        assert!(
            value < previous,
            "frame {frame} went the wrong way: {value}"
        );
        previous = value;
    }
}

#[test]
fn a_layer_at_full_opacity_is_not_touched() {
    // Every frame outside a dissolve goes through the same code path, so the
    // path has to be a copy rather than a multiply by one — or every ordinary
    // frame in the programme would be a rounding away from its source.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [3, 5, 7, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(4),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (3, 5, 7, 255));
}

#[test]
fn a_pool_kept_across_renders_stops_the_same_frame_being_fetched_twice() {
    // The reason the render is a graph rather than a loop. A node's key is a
    // digest over its kind, its parameters and its inputs' identities, so a
    // pool that outlives one render answers for anything it has already seen —
    // and for a `Source` node the cost avoided is a decode.
    //
    // Scrubbing back and forth over one instant is the case a user creates
    // constantly, and it must not decode again each time.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [10, 20, 30, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut kept = pool();

    let first = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(source.asked.len(), 1);

    let second = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(first, second, "and the answer is the same one");
    assert_eq!(
        source.asked.len(),
        1,
        "the second render decoded nothing at all"
    );

    // A different instant of the same clip is a different frame, so it is
    // fetched — the cache must not be answering by luck.
    timeline::render(
        &project,
        sequence,
        at(4),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(source.asked.len(), 2);
}

#[test]
fn two_sequences_using_one_asset_share_its_cached_frames() {
    // The graph names media by what it *is* rather than by this project's
    // index for it. Two sequences cutting the same footage therefore hit the
    // same cache entry — and a source that recorded a second fetch would show
    // that the naming had gone project-local somewhere.
    let mut project = Project::new();
    let id = media(&mut project, 1);
    let one = project.add_sequence(RATE).expect("a sequence");
    let two = project.add_sequence(RATE).expect("a sequence");
    for sequence in [one, two] {
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
        );
    }
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [1, 2, 3, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut kept = pool();

    let from_one = timeline::render(&project, one, at(2), described(), &mut kept, &mut source)
        .expect("a render");
    let from_two = timeline::render(&project, two, at(2), described(), &mut kept, &mut source)
        .expect("a render");
    assert_eq!(from_one, from_two);
    assert_eq!(source.asked.len(), 1, "one decode served both sequences");
}

#[test]
fn a_plan_can_be_read_without_fetching_anything() {
    // Building the graph and evaluating it are separate questions, and a
    // caller may want only the first: an export deciding whether an instant
    // needs decoding at all has no frames to fetch.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(one, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(two, 0, frames(10)).expect("a clip"))],
    );

    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, root) =
        timeline::plan(&project, sequence, at(5), described(), &mut source).expect("a plan");
    // A blank, two sources, and two `over` nodes: five.
    assert_eq!(graph.len(), 5);
    assert_eq!(
        graph.description(root).expect("a description"),
        described(),
        "and the root produces exactly what was asked for"
    );

    // An empty instant plans to a blank and nothing else.
    let (graph, _) =
        timeline::plan(&project, sequence, at(50), described(), &mut source).expect("a plan");
    assert_eq!(graph.len(), 1);
}

/// A table that swaps red and blue, neutral on its own diagonal.
fn swap_look() -> Look {
    use sapstudio_render::lut::Lut3D;
    let size = 5_usize;
    let last = 4_i64;
    let mut samples = std::vec::Vec::new();
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let value = |axis: usize| {
                    sapstudio_core::Fixed::from_rational(
                        sapstudio_core::Rational::new(i64::try_from(axis).expect("an index"), last)
                            .expect("a ratio"),
                    )
                    .expect("a value")
                };
                samples.push([value(blue), value(green), value(red)]);
            }
        }
    }
    Look::new(
        Lut3D::new(size, samples).expect("a table"),
        // The straight description the plan fetches a graded layer in.
        described()
            .with_alpha(AlphaState::Straight)
            .expect("a description")
            .colour(),
        sapstudio_render::lut::Interpolation::Tetrahedral,
    )
}

#[test]
fn a_graded_clip_is_graded_when_it_renders() {
    // The end-to-end case, and the one a negative control found missing: the
    // model carried a grade, the node existed, and nothing tested that `plan`
    // put one in front of the other. Removing the wiring broke no test at all
    // until this one existed.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );

    let look = swap_look();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look.digest().expect("a digest")),
            },
        )
        .expect("a grade");

    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [200, 40, 10, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec![(look.digest().expect("a digest"), look)],
        answers_wrongly: false,
    };
    let frame = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a frame");

    // Red and blue swapped, opaque, over black.
    assert_eq!(
        pixel(&frame),
        (10, 40, 200, 255),
        "the grade did not reach the picture"
    );
}

#[test]
fn a_graded_layer_is_fetched_straight_and_associated_afterwards() {
    // A look is a non-linear function and on premultiplied samples computes
    // `f(ac)` where `a·f(c)` was wanted; `over` is only correct on
    // premultiplied ones. The two want opposite things, so a graded layer
    // arrives the way the look needs it and is associated afterwards — which
    // loses nothing, since it was never premultiplied to begin with.
    //
    // Without that, a graded clip cannot render at all: the look refuses the
    // frame the compositor wants. This asserts the *fetch*, because the
    // rendering test above would pass for a pipeline that unpremultiplied
    // silently, and unpremultiplying is lossy.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let look = swap_look();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look.digest().expect("a digest")),
            },
        )
        .expect("a grade");

    let mut library = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, root) =
        timeline::plan(&project, sequence, at(0), described(), &mut library).expect("a plan");

    // Walk from the root down its own edges, rather than over the node list:
    // a graph exposes no way to name a node by index, and adding one so a test
    // could enumerate would be public surface bought for a test.
    let mut straight = 0;
    let mut looks = 0;
    let mut associations = 0;
    let mut pending = std::vec![root];
    while let Some(id) = pending.pop() {
        let node = graph.node(id).expect("a node");
        pending.extend_from_slice(node.inputs());
        match node {
            sapstudio_render::Node::Source { description, .. } => {
                assert_eq!(
                    description.alpha(),
                    Some(AlphaState::Straight),
                    "a graded layer was fetched premultiplied, which the look refuses"
                );
                straight += 1;
            }
            sapstudio_render::Node::Look { .. } => looks += 1,
            sapstudio_render::Node::Associate { target, .. } => {
                assert_eq!(
                    target.alpha(),
                    Some(AlphaState::Premultiplied),
                    "the association did not put it back for the compositor"
                );
                associations += 1;
            }
            _ => {}
        }
    }
    assert_eq!(straight, 1, "the source was not fetched straight");
    assert_eq!(looks, 1, "no look reached the graph");
    assert_eq!(associations, 1, "the layer was never re-associated");
    assert_eq!(
        graph.description(root).expect("a description").alpha(),
        Some(AlphaState::Premultiplied),
        "the render does not end premultiplied"
    );
}

#[test]
fn a_wipe_puts_a_hard_edge_across_the_picture() {
    // The end of the chain: a wipe in the model becomes a coverage plane in
    // the renderer and an edge in the picture. On the swept side the incoming
    // clip is whole; on the other, the outgoing one is; and the frame is
    // opaque all the way across, because the programme is.
    use sapstudio_model::{Transition, Wipe};

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let outgoing = media(&mut project, 1);
    let incoming = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
            Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
        ],
    );
    // Four frames, centred on the cut at frame 10: 8, 9, 10, 11. At frame 9
    // the fraction is 2/5, so on a four-pixel-wide frame the edge sits at 1.6.
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe"),
            },
        )
        .expect("a wipe");
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, outgoing), [255, 255, 255, 255]),
            (digest_of(&project, incoming), [0, 0, 0, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(9),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let bytes = rendered.to_packed().expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();

    // The incoming clip is black and fully covers the first pixel, so it is
    // black; the last two are past the edge and stay white. The second is the
    // one the edge crosses, and it is neither.
    assert_eq!(row[0], 0, "wholly the incoming clip");
    assert_eq!(row[3], 255, "wholly the outgoing clip");
    assert!(
        row[1] > 0 && row[1] < 255,
        "the pixel the edge crosses is neither, got {}",
        row[1]
    );
    assert_eq!(row[2], 255, "still past the edge at 1.6");
    for column in 0..4 {
        assert_eq!(
            bytes[column * 4 + 3],
            255,
            "the programme is opaque across the edge, at {column}"
        );
    }
}

#[test]
fn a_wipe_and_a_dissolve_are_different_pictures() {
    // They are timed identically and stack identically, so nothing before the
    // renderer can tell them apart. This is the test that the difference
    // survives all the way to the pixels -- without it, the whole transition
    // kind could be ignored and every test above would still pass.
    use sapstudio_model::{Transition, Wipe};

    let build = |wiping: bool| {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let outgoing = media(&mut project, 1);
        let incoming = media(&mut project, 2);
        lay(
            &mut project,
            sequence,
            0,
            &[
                Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
                Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
            ],
        );
        let transition = if wiping {
            Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe")
        } else {
            Transition::new(1, frames(4)).expect("a dissolve")
        };
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition,
                },
            )
            .expect("a transition");
        let mut source = Flat {
            colours: std::vec![
                (digest_of(&project, outgoing), [255, 255, 255, 255]),
                (digest_of(&project, incoming), [0, 0, 0, 255]),
            ],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        timeline::render(
            &project,
            sequence,
            at(9),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render")
        .to_packed()
        .expect("bytes")
    };

    let wiped = build(true);
    let dissolved = build(false);
    assert_ne!(wiped, dissolved);
    // And the difference is the shape of it: a dissolve is the same value at
    // every pixel of the row, a wipe is not.
    let row = |bytes: &[u8]| (0..4).map(|c| bytes[c * 4]).collect::<std::vec::Vec<u8>>();
    let flat = row(&dissolved);
    assert!(
        flat.iter().all(|value| *value == flat[0]),
        "a dissolve is uniform across the frame, got {flat:?}"
    );
    let edged = row(&wiped);
    assert!(
        edged.iter().any(|value| *value != edged[0]),
        "a wipe is not, got {edged:?}"
    );
}

#[test]
fn a_soft_wipe_has_more_than_one_pixel_between_the_two_clips() {
    // The difference a soft edge makes, at the only place it can be seen: a
    // hard wipe has exactly one partial pixel across a row, because a straight
    // line crosses one pixel per row. A soft one has a band of them.
    use sapstudio_core::Rational;
    use sapstudio_model::{Transition, Wipe};

    let build = |softness: Rational| {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let outgoing = media(&mut project, 1);
        let incoming = media(&mut project, 2);
        lay(
            &mut project,
            sequence,
            0,
            &[
                Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
                Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
            ],
        );
        let wipe = Wipe::soft(Rational::ONE, Rational::ZERO, softness).expect("a wipe");
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition: Transition::wiping(1, frames(4), wipe).expect("a wipe"),
                },
            )
            .expect("a wipe");
        let mut source = Flat {
            colours: std::vec![
                (digest_of(&project, outgoing), [255, 255, 255, 255]),
                (digest_of(&project, incoming), [0, 0, 0, 255]),
            ],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        let bytes = timeline::render(
            &project,
            sequence,
            at(9),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render")
        .to_packed()
        .expect("bytes");
        (0..4)
            .map(|column| bytes[column * 4])
            .collect::<std::vec::Vec<u8>>()
    };

    let hard = build(Rational::ZERO);
    let soft = build(Rational::new(3, 4).expect("a softness"));
    assert_ne!(hard, soft);
    let between = |row: &[u8]| {
        row.iter()
            .filter(|value| **value > 0 && **value < 255)
            .count()
    };
    assert_eq!(
        between(&hard),
        1,
        "a line crosses one pixel per row: {hard:?}"
    );
    assert!(between(&soft) > 1, "a ramp crosses several, got {soft:?}");
    // Still monotone across the row, and still opaque -- a soft edge changes
    // how the two clips meet, not which is on which side.
    assert!(
        soft.windows(2).all(|pair| pair[0] <= pair[1]),
        "the row runs from the incoming clip to the outgoing one: {soft:?}"
    );
}

#[test]
fn a_mask_takes_away_everything_outside_its_shape() {
    // The end of the chain for a mask: a shape in the model becomes coverage
    // in the renderer and a hole in the picture. Outside the shape the layer
    // is gone and the black underneath shows; inside it, the clip is whole.
    use sapstudio_core::Rational;
    use sapstudio_model::Mask;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
    );
    // The left half of a four-wide frame.
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::new(1, 2).expect("a half"),
        Rational::ONE,
    )
    .expect("a rectangle");
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
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let bytes = rendered.to_packed().expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert_eq!(row, std::vec![255, 255, 0, 0], "the left half survives");
    for column in 0..4 {
        assert_eq!(
            bytes[column * 4 + 3],
            255,
            "the programme is opaque wherever the mask cut, at {column}"
        );
    }
}

#[test]
fn an_inverted_mask_keeps_the_other_half() {
    use sapstudio_core::Rational;
    use sapstudio_model::Mask;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
    );
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::new(1, 2).expect("a half"),
        Rational::ONE,
    )
    .expect("a rectangle")
    .inverted();
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
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let bytes = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render")
    .to_packed()
    .expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert_eq!(row, std::vec![0, 0, 255, 255], "the other half survives");
}

/// A library that can be told which media it cannot reach.
struct Sometimes {
    inner: Flat,
    missing: std::vec::Vec<Digest>,
}

impl Library for Sometimes {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        assert!(
            !self.missing.contains(&media),
            "the planner asked for media it had been told was not there"
        );
        self.inner.frame(media, tick, description)
    }

    fn available(&mut self, media: Digest) -> bool {
        !self.missing.contains(&media)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.inner.look(look)
    }
}

/// One clip of one media, and a library that may or may not have it.
fn one_clip() -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let only = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(only, 0, frames(10)).expect("a clip"))],
    );
    (project, sequence, only)
}

#[test]
fn a_clip_whose_media_is_missing_still_renders() {
    // A project opens when the drive is not mounted. Failing the whole render
    // because one source is unreachable is what makes an editor unusable on
    // the day it matters most.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render even though the media is gone");
    assert_eq!(
        rendered.description(),
        &described(),
        "and it is still exactly the description that was asked for"
    );
}

#[test]
fn offline_is_not_black_and_is_not_a_colour_a_camera_makes() {
    // Black is what an empty timeline shows and a solid colour is something a
    // programme might legitimately contain, so either one would let "the drive
    // is not mounted" look like footage.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let bytes = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render")
    .to_packed()
    .expect("bytes");
    let reds: std::collections::BTreeSet<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert!(
        reds.len() > 1,
        "the offline slate varies across the frame, got {reds:?}"
    );
    assert!(
        bytes.chunks_exact(4).all(|pixel| pixel[3] == 255),
        "and the programme is still opaque"
    );
}

#[test]
fn the_offline_slate_never_reaches_the_cache_under_the_pictures_key() {
    // The reason availability is asked *before* the graph is built. A source
    // node's identity covers the media, the tick and the description and not
    // whether the file happened to be reachable -- so a node that fell back
    // during evaluation would cache the slate under the real picture's key and
    // hand it back once the drive came home.
    //
    // One pool across both renders, which is exactly the condition that would
    // expose it.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut shared = pool();

    let mut absent = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let offline = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut shared,
        &mut absent,
    )
    .expect("a render");

    let mut present = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec::Vec::new(),
    };
    let restored = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut shared,
        &mut present,
    )
    .expect("a render");

    assert_ne!(
        offline.digest(),
        restored.digest(),
        "the drive came home and the picture came back"
    );
    assert_eq!(pixel(&restored), (200, 30, 40, 255));
}

#[test]
fn a_planner_does_not_ask_for_media_it_was_told_is_missing() {
    // The `Sometimes` library panics if asked, so this asserting nothing extra
    // is the assertion: an unavailable source is never named in the graph, so
    // nothing ever tries to fetch it.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec::Vec::new(),
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let (graph, _) =
        timeline::plan(&project, sequence, at(5), described(), &mut library).expect("a plan");
    assert_eq!(graph.len(), 3, "a blank, a slate, and one `over`");
    timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
}
