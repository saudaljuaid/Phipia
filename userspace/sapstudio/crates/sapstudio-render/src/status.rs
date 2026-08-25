// SPDX-License-Identifier: GPL-3.0-only
//! Every way the colour pipeline refuses.

use sapstudio_core::CoreStatus;
use sapstudio_media::MediaStatus;

/// A refusal from the render side.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum RenderStatus {
    /// An exact arithmetic refusal from the core types.
    Time(CoreStatus),
    /// The media types refused.
    Media(MediaStatus),
    /// An allocation the caller must handle.
    OutOfMemory,
    /// A matrix has no inverse.
    Singular,
    /// A chromaticity's y coordinate is zero, so it names no colour.
    DegenerateChromaticity,
    /// A set of primaries and a white point that do not span a gamut.
    DegenerateGamut,
    /// A conversion between two descriptions that this build cannot do
    /// exactly.
    ConversionUnavailable,
    /// A value outside the domain a transfer function defines.
    OutsideDomain,
    /// A node identifier names nothing in the graph.
    UnknownNode,
    /// A graph is at its policy capacity.
    GraphTooLarge,
    /// Compositing was asked of a format with no alpha channel.
    AlphaRequired,
    /// A frame is straight where premultiplied was required, or the reverse.
    WrongAlphaState,
    /// A frame calls itself premultiplied and holds colour brighter than its
    /// own coverage, which premultiplied samples cannot.
    NotPremultiplied,
    /// Two frames that do not describe the same picture cannot be layers of
    /// one.
    NotComposable,
    /// A lookup table with a side outside the range this build carries.
    LutSizeUnsupported,
    /// A lookup table whose sample count is not the cube of its size.
    LutNotACube,
    /// A lattice point outside a lookup table.
    LutIndexOutOfRange,
    /// A frame in a different encoding from the one a look was authored for.
    LookSpaceMismatch,
    /// A look asked of a format that is not red-green-blue.
    LookNotRgb,
    /// A look asked of premultiplied coverage, where a non-linear function
    /// computes the wrong thing at every coverage but full.
    LookPremultiplied,
    /// A source answered with a frame that is not the one asked for.
    SourceDescriptionMismatch,
    /// An edge whose coefficients name no line.
    DegenerateEdge,
    /// A shape with no edges, or one enclosing no area.
    DegenerateShape,
    /// A shape with more edges than the rasteriser carries.
    ShapeTooComplex,
    /// A coverage plane whose size does not match the frame it is for.
    CoverageSizeMismatch,
}

impl RenderStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::Media(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::Singular => "the matrix has no inverse",
            Self::DegenerateChromaticity => "a chromaticity with a zero y names no colour",
            Self::DegenerateGamut => "those primaries and that white point span no gamut",
            Self::ConversionUnavailable => "this build cannot do that conversion exactly",
            Self::OutsideDomain => "that value is outside the transfer function's domain",
            Self::UnknownNode => "the identifier names no node in this graph",
            Self::GraphTooLarge => "the graph is at its capacity",
            Self::AlphaRequired => "compositing needs an alpha channel and this format has none",
            Self::WrongAlphaState => "the frame's alpha association is not the one required",
            Self::NotPremultiplied => "this frame's colour is brighter than its own coverage",
            Self::NotComposable => "these two frames do not describe the same picture",
            Self::LutSizeUnsupported => "that lookup table side is not one this build carries",
            Self::LutNotACube => "that many samples is not the cube of that size",
            Self::LutIndexOutOfRange => "that lattice point is outside the table",
            Self::LookSpaceMismatch => "that frame is not in the encoding this look was made for",
            Self::LookNotRgb => "a look maps three colour channels, and that format has other ones",
            Self::LookPremultiplied => "a look needs straight coverage, not premultiplied",
            Self::SourceDescriptionMismatch => "the source answered with a different frame",
            Self::DegenerateEdge => "an edge with no direction names no line",
            Self::DegenerateShape => "that shape encloses nothing",
            Self::ShapeTooComplex => "that shape has more edges than this rasterises",
            Self::CoverageSizeMismatch => "the coverage plane is not the size of the frame",
        }
    }
}

impl From<CoreStatus> for RenderStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}

impl From<MediaStatus> for RenderStatus {
    fn from(status: MediaStatus) -> Self {
        Self::Media(status)
    }
}

impl core::fmt::Display for RenderStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, RenderStatus>;
