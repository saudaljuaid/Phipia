// SPDX-License-Identifier: GPL-3.0-only
//! Turning a stack of layers into a picture.
//!
//! [`sapstudio_model::Sequence::stack_at`] answers what is on each track at an
//! instant. This answers what that *looks* like, and the two are deliberately
//! separate: the first is a fact about the project, the second is a policy
//! about rendering, and the policy lives here because the application is the
//! part that owns decisions.
//!
//! Two of those decisions are worth naming.
//!
//! **The programme is opaque.** A sequence with nothing at an instant shows
//! black, not a hole — a viewer displays black leader, an export writes black
//! frames, and neither shows whatever was behind the window. So the stack is
//! composited onto an opaque black base, and the result is opaque whatever the
//! layers were.
//!
//! **Layers composite bottom first**, in the order the stack comes back, each
//! one `over` what is beneath it. That is [`sapstudio_render::over`], which
//! means it happens in linear light and only on premultiplied values — so a
//! title with a soft edge on V2 lands on V1 without a fringe, and it lands
//! there identically on every machine.
//!
//! A dissolve needs no second operator. The model reports both sides of the
//! cut, the outgoing one at full opacity and the incoming one at the fraction
//! of the way through — so `over` computes `in x t + out x (1 - t)`, which is
//! what a cross-fade is. Opacity scales a layer's coverage, and scaling a
//! premultiplied frame's coverage means scaling its colour by the same amount,
//! because that is what premultiplied means.
//!
//! Where the frames come from is a [`sapstudio_render::Media`], because on
//! Sapote today there is nowhere to read media from (`SAP-08`). A test
//! provides one that draws flat colours; a real session will provide one that
//! decodes, and neither this module nor the model changes when it does.
//!
//! The render is expressed as a **graph** rather than performed directly, and
//! that is not ceremony. A node's cache key is a digest over its kind, its
//! parameters and its inputs' identities, so the pool answers for anything the
//! render already has — most usefully a `Source`, where the cost is a decode.
//! Scrub back over a cut and nothing is decoded twice; a dissolve that reaches
//! the same frame from both sides fetches it once.
//!
//! The graph also names media by **what it is** rather than by this project's
//! index for it, so the same footage in two sequences shares one cached frame,
//! and a file swapped underneath is a different key rather than a stale hit.

use alloc::vec::Vec;

use sapstudio_core::Instant;
use sapstudio_media::{AlphaState, Frame, FrameDescription, FramePool, TestPattern};
use sapstudio_model::{Lane, Layer, Project, Sequence, SequenceId};
use sapstudio_render::{Graph, Library, Node, NodeId};

use crate::SlateStatus;

/// Build the graph that renders one instant of a sequence.
///
/// Separate from evaluating it, because they are separate questions and a
/// caller may want only the first: an export that wants to know whether an
/// instant needs decoding at all, or an interface drawing what is on the
/// timeline, has no frames to fetch.
///
/// # Errors
///
/// [`SlateStatus::Model`] if the instant is not in the sequence's timebase or
/// the sequence is not in the project, [`SlateStatus::BaseNotPremultiplied`]
/// if the description is not one this can composite onto, and
/// [`SlateStatus::Render`] if the graph is at capacity.
pub fn plan(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    library: &mut dyn Library,
) -> Result<(Graph, NodeId), SlateStatus> {
    if description.alpha() != Some(AlphaState::Premultiplied) {
        // Compositing is only correct on premultiplied values, so the base of
        // the stack has to be one. Refusing rather than converting keeps the
        // decision where the caller made it (R-1.3).
        return Err(SlateStatus::BaseNotPremultiplied);
    }
    let held = project.sequence(sequence)?;
    let stack = held.stack_at(Lane::Picture, instant)?;

    let mut graph = Graph::new();
    // The programme is opaque: an empty instant is black leader, not a hole. A
    // viewer shows black and an export writes it, and neither shows whatever
    // was behind the window.
    let mut top = graph.add(Node::Blank { description })?;
    for layer in &stack {
        let asset = project.media().get(layer.media())?;
        // A graded layer is fetched *straight*. A look is a non-linear
        // function and on premultiplied samples computes `f(ac)` where
        // `a·f(c)` was wanted; `over` is only correct on premultiplied ones.
        // The two want opposite things, so the frame arrives the way the look
        // needs it and is associated afterwards — which loses nothing, since
        // it was never premultiplied to begin with. Unpremultiplying a frame
        // that already had been would.
        let fetched = match layer.grade() {
            None => description,
            Some(_) => description
                .with_alpha(AlphaState::Straight)
                .map_err(SlateStatus::Media)?,
        };
        // Ask whether the bytes are there *before* naming them, rather than
        // finding out while evaluating. A source node's identity covers the
        // media, the tick and the description and not whether the file
        // happened to be reachable — so a node that fell back to a slate
        // during evaluation would put that slate in the cache under the real
        // picture's key and hand it back after the drive came home. The
        // fallback belongs here, where nothing is cached by it.
        let source = if library.available(asset.digest()) {
            graph.add(Node::Source {
                media: asset.digest(),
                tick: layer.source(),
                description: fetched,
            })?
        } else {
            graph.add(Node::Pattern {
                pattern: TestPattern::Offline,
                description: fetched,
            })?
        };
        // The grade goes on before the fade, and that order is a decision. A
        // look is a function of colour; fading is a statement about how much
        // of a layer is showing. Grading a half-faded layer would ask the
        // table about a colour that is neither on the screen nor in the clip.
        // So: graded, then shown at whatever opacity it is at.
        let graded = match layer.grade() {
            None => source,
            Some(look) => {
                let looked = graph.add(Node::Look {
                    input: source,
                    look,
                })?;
                graph.add(Node::Associate {
                    input: looked,
                    target: description,
                })?
            }
        };
        // The mask goes on after the grade and before the fade, and both
        // halves of that are decisions. After the grade, because a mask is
        // about *where* and a look is about colour, and grading only the part
        // that survives would ask the table about a picture nobody assembled.
        // Before the fade, because the mask says what this clip *is* and the
        // fade says how much of the track is showing — and a fade applied
        // first would then be masked away in the places the mask cuts, which
        // is the same number by accident and the wrong order in principle.
        let shaped = match layer.mask() {
            None => graded,
            Some(mask) => graph.add(Node::Mask {
                input: graded,
                corners: mask.corners().to_vec(),
                inverted: mask.is_inverted(),
            })?,
        };
        let faded = if layer.opacity() == sapstudio_core::Rational::ONE {
            shaped
        } else {
            graph.add(Node::Fade {
                input: shaped,
                opacity: layer.opacity(),
            })?
        };
        // A wipe after the fade, and that order is the same decision the grade
        // makes in the other direction. The fade is the track's automation,
        // which says how much of this track is showing at all; the wipe is the
        // transition, which says how much of *this clip* has been revealed.
        // Wiping first and then fading would give the same picture here, since
        // both are multiplications by a coverage — but only because neither is
        // a function of colour, and putting the one that is (the grade) in the
        // wrong place is exactly the mistake this ordering exists to avoid.
        let revealed = match layer.wipe() {
            None => faded,
            Some(sweep) => graph.add(Node::Wipe {
                input: faded,
                across: sweep.wipe().across(),
                down: sweep.wipe().down(),
                fraction: sweep.fraction(),
                softness: sweep.wipe().softness(),
            })?,
        };
        top = graph.add(Node::Over {
            layers: [top, revealed],
        })?;
    }
    Ok((graph, top))
}

/// Render one instant of a sequence.
///
/// The picture tracks are composited bottom first onto opaque black in
/// `description`, so the result is always exactly that description and always
/// opaque — a viewer never has to ask what it is looking at.
///
/// # Errors
///
/// As [`plan`], plus [`sapstudio_render::RenderStatus::SourceDescriptionMismatch`]
/// if a source hands back a frame that is not what was asked for, and whatever
/// the source itself refuses.
pub fn render(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    pool: &mut FramePool,
    library: &mut dyn Library,
) -> Result<Frame, SlateStatus> {
    let (graph, root) = plan(project, sequence, instant, description, library)?;
    Ok(graph.evaluate(root, pool, library)?)
}

/// What a sequence shows at an instant, without rendering it.
///
/// Useful on its own: this is what a timeline draws in its own interface, and
/// what an export uses to decide whether an instant needs decoding at all.
///
/// # Errors
///
/// As [`sapstudio_model::Sequence::stack_at`].
pub fn stack(sequence: &Sequence, instant: Instant) -> Result<Vec<Layer>, SlateStatus> {
    Ok(sequence.stack_at(Lane::Picture, instant)?)
}
