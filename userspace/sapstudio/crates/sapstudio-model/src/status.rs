// SPDX-License-Identifier: GPL-3.0-only
//! Every way the model refuses.

use sapstudio_core::CoreStatus;

/// A refusal from the project model.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ModelStatus {
    /// An arithmetic or timebase refusal from the core types.
    Time(CoreStatus),
    /// An allocation that the caller must handle rather than abort on.
    OutOfMemory,
    /// A fixed policy capacity is reached.
    CapacityExhausted,
    /// A track index names no track.
    UnknownTrack,
    /// An item index names no item.
    UnknownItem,
    /// A media identifier names nothing in the library.
    UnknownMedia,
    /// A sequence identifier names nothing in the project.
    UnknownSequence,
    /// A media asset is counted in a timebase the sequence does not share.
    MediaTimebaseMismatch,
    /// An item's length would become zero or negative.
    EmptyItem,
    /// A split was asked for at the very start or end of an item, which would
    /// produce an empty piece.
    SplitOutsideItem,
    /// Two items cannot be joined: different kinds, different media, or a gap
    /// in the source between them.
    ItemsNotContiguous,
    /// A clip's source range would run off the start of its media.
    SourceBeforeStart,
    /// A clip's source range would run past the end of its media.
    SourceAfterEnd,
    /// A track that still holds items cannot be removed.
    TrackNotEmpty,
    /// There is nothing left to undo, or nothing left to redo.
    NothingToDo,
    /// A wipe with no direction, which would leave its edge nowhere.
    DegenerateWipe,
    /// A wipe softness outside nought to one.
    SoftnessOutOfRange,
    /// A mask with too few corners to enclose anything.
    MaskTooSimple,
    /// A mask whose corners turn both ways.
    MaskNotConvex,
    /// Two records of one digest that describe it differently.
    MediaContradiction,
    /// A location hint with no bytes in it.
    EmptyLocation,
    /// An operation that only a clip supports was asked of a gap.
    NotAClip,
    /// A fader set past the ends of its own travel.
    FaderOutOfRange,
    /// A dissolve was asked for at a cut that already has one.
    TransitionExists,
    /// A dissolve longer than one of the clips it dissolves between.
    TransitionTooLong,
    /// A cut named no dissolve.
    UnknownTransition,
    /// An edit would renumber a cut that a dissolve sits on. Take the dissolve
    /// off first.
    TransitionInTheWay,
    /// Undoing an edit did not reproduce the edit that was applied, so the
    /// model and its history describe different projects.
    HistoryInconsistent,
    /// Media cannot be removed while a sequence still cuts from it.
    MediaInUse,
    /// A curve with no keyframes, which could not say what the parameter is.
    EmptyCurve,
    /// Keyframes that do not run strictly forward in time — which covers two
    /// at one instant, because a parameter with two values at one moment has
    /// none.
    KeyframesOutOfOrder,
    /// Keyframes counted in more than one timebase.
    MixedTimebases,
    /// An instant counted differently from the thing it was asked about.
    WrongTimebase,
    /// An ease handle outside the span between its keyframes, which makes a
    /// curve that goes back in time.
    HandleOutOfSpan,
    /// An opacity was set on a sound track, whose level is its fader.
    OpacityOnSound,
    /// A fader level was set on a picture track, whose level is its opacity.
    LevelOnPicture,
    /// No keyframe sits at that instant.
    NoSuchKeyframe,
    /// A keyframe already sits at that instant, and a parameter with two
    /// values at one moment has none.
    KeyframeExists,
    /// The last keyframe cannot be taken out of a curve; turning the
    /// automation off is a different operation.
    LastKeyframe,
    /// That lane has no automation to change.
    NoAutomation,
}

impl ModelStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::CapacityExhausted => "a policy capacity is reached",
            Self::UnknownTrack => "there is no track at that index",
            Self::UnknownItem => "there is no item at that index",
            Self::UnknownMedia => "the media identifier names nothing",
            Self::UnknownSequence => "the sequence identifier names nothing",
            Self::MediaTimebaseMismatch => "the media is counted in another timebase",
            Self::EmptyItem => "an item cannot have zero length",
            Self::SplitOutsideItem => "the split point is not inside the item",
            Self::ItemsNotContiguous => "the two items are not contiguous in their source",
            Self::SourceBeforeStart => "the source range starts before the media does",
            Self::SourceAfterEnd => "the source range runs past the end of the media",
            Self::TrackNotEmpty => "the track still holds items",
            Self::NothingToDo => "there is nothing to undo or redo",
            Self::DegenerateWipe => "a wipe with no direction has nowhere to put its edge",
            Self::SoftnessOutOfRange => "a wipe fades over none to all of its travel",
            Self::MaskTooSimple => "those corners enclose no area",
            Self::MaskNotConvex => {
                "a concave mask is a union of convex ones, which this does not build"
            }
            Self::MediaContradiction => "the same content cannot be two different lengths",
            Self::EmptyLocation => "a hint that says nothing is worse than none",
            Self::NotAClip => "that operation applies only to a clip",
            Self::FaderOutOfRange => "that level is past the ends of the fader",
            Self::TransitionExists => "that cut already has a dissolve on it",
            Self::TransitionTooLong => "a dissolve may not outlast the clips it is between",
            Self::UnknownTransition => "that cut has no dissolve on it",
            Self::TransitionInTheWay => "this edit would move a dissolve; take it off first",
            Self::HistoryInconsistent => "the model and its history have diverged",
            Self::MediaInUse => "a sequence still cuts from that media",
            Self::EmptyCurve => "a curve with no keyframes has no value to give",
            Self::KeyframesOutOfOrder => "keyframes must run strictly forward in time",
            Self::MixedTimebases => "these are not all counted the same way",
            Self::WrongTimebase => "that instant is counted another way",
            Self::HandleOutOfSpan => "an ease handle sits outside the span it eases across",
            Self::OpacityOnSound => "a sound track's level is its fader, not an opacity",
            Self::LevelOnPicture => "a picture track's level is its opacity, not a fader",
            Self::NoSuchKeyframe => "no keyframe sits at that instant",
            Self::KeyframeExists => "a keyframe already sits at that instant",
            Self::LastKeyframe => "a curve cannot lose its last keyframe",
            Self::NoAutomation => "that lane has no automation to change",
        }
    }
}

impl From<CoreStatus> for ModelStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}

impl core::fmt::Display for ModelStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, ModelStatus>;
