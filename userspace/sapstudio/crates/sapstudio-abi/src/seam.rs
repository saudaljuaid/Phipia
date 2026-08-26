// SPDX-License-Identifier: GPL-3.0-only
//! The five seams.
//!
//! Everything the platform provides reaches SapStudio through exactly five
//! interfaces, and no more: presentation, input, storage, time, and audio.
//! Five is a rule, not a count — a sixth would be the beginning of a general
//! "system" interface, and a general system interface is how an application
//! stops being native to anything.
//!
//! Two are defined here because two are all that `SAP-01` supports. The rest
//! arrive with the capabilities they need, each one in this module, each one
//! with a Sapote implementation and a deterministic test implementation.

use core::fmt::Write;

/// A refusal from a seam.
///
/// Deliberately coarse: a seam's job is to be a boundary, and an application
/// that wants to distinguish twelve kinds of console failure has put logic in
/// the wrong place.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SeamStatus {
    /// The kernel refused the request.
    Refused,
    /// The request was larger than the seam accepts.
    TooLarge,
    /// The capability this seam needs does not exist on this kernel.
    Unavailable,
    /// The slot holds nothing.
    Empty,
}

/// The result of a seam operation.
pub type Result<T> = core::result::Result<T, SeamStatus>;

/// Somewhere to write diagnostics.
///
/// On Sapote this is the kernel's console and serial transcript. In the host
/// suite it is a buffer a test compares against, which is what makes an
/// application's whole output checkable without an emulator.
pub trait Console {
    /// Write bytes, all of them or none.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::TooLarge`] past whatever the implementation accepts in
    /// one call, or [`SeamStatus::Refused`].
    fn write(&mut self, bytes: &[u8]) -> Result<()>;

    /// Write a line, followed by a newline.
    ///
    /// # Errors
    ///
    /// As [`Console::write`].
    fn write_line(&mut self, text: &str) -> Result<()> {
        self.write(text.as_bytes())?;
        self.write(b"\n")
    }
}

/// An adapter that lets `write!` target a [`Console`].
///
/// Formatting cannot report a seam refusal through [`core::fmt`], so this
/// records the first one and hands it back at the end rather than losing it
/// (R-7.4).
pub struct ConsoleWriter<'a, C: Console + ?Sized> {
    console: &'a mut C,
    failure: Option<SeamStatus>,
}

impl<'a, C: Console + ?Sized> ConsoleWriter<'a, C> {
    /// Wrap a console for formatted output.
    pub fn new(console: &'a mut C) -> Self {
        Self {
            console,
            failure: None,
        }
    }

    /// The first refusal that occurred, if any.
    ///
    /// # Errors
    ///
    /// Whatever the console refused.
    pub fn finish(self) -> Result<()> {
        self.failure.map_or(Ok(()), Err)
    }
}

impl<C: Console + ?Sized> Write for ConsoleWriter<'_, C> {
    fn write_str(&mut self, text: &str) -> core::fmt::Result {
        match self.console.write(text.as_bytes()) {
            Ok(()) => Ok(()),
            Err(status) => {
                self.failure.get_or_insert(status);
                Err(core::fmt::Error)
            }
        }
    }
}

/// Which of the two fixed storage slots an operation names.
///
/// Sapote has no paths, no directories, and no rename (`SAP-08`), so the
/// smallest storage that can hold a project safely is two fixed extents and an
/// operation that swaps them. That is deliberately less than a filesystem: it
/// is exactly enough to save a file without ever being able to lose the
/// previous one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Slot {
    /// The project as it was last committed. Never written directly.
    Project,
    /// Where a save is assembled and verified before it is committed.
    Scratch,
}

/// Two fixed extents and an atomic swap between them.
///
/// The contract that matters is [`Storage::commit`]: until it returns, the
/// `Project` slot holds exactly what it held before, whatever happened to the
/// scratch slot. That is what makes R-9.4 provable rather than hoped for.
pub trait Storage {
    /// The most bytes a slot can hold.
    fn capacity(&self, slot: Slot) -> usize;

    /// How many bytes the slot currently holds.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`] if nothing has been written to it.
    fn len(&self, slot: Slot) -> Result<usize>;

    /// Copy the whole slot into `into`, and say how many bytes that was.
    ///
    /// All or nothing: a destination too small for the stored bytes is
    /// refused rather than partly filled (R-1.4).
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`], or [`SeamStatus::TooLarge`] if `into` is
    /// smaller than the stored length.
    fn read(&self, slot: Slot, into: &mut [u8]) -> Result<usize>;

    /// Replace a slot's contents.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::TooLarge`] past the slot's capacity, or
    /// [`SeamStatus::Refused`].
    fn write(&mut self, slot: Slot, bytes: &[u8]) -> Result<()>;

    /// Make the scratch slot the project, in one step that either happens or
    /// does not.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`] if nothing has been written to the scratch slot,
    /// or [`SeamStatus::Refused`].
    fn commit(&mut self) -> Result<()>;
}

/// The kernel's one monotonic clock.
pub trait Time {
    /// Nanoseconds since an unspecified origin, never going backwards.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Unavailable`] where `SAP-05` does not exist.
    fn monotonic_nanoseconds(&self) -> Result<u64>;
}
