// SPDX-License-Identifier: GPL-3.0-only
//! Saving, without ever being able to lose the last one.
//!
//! R-9.4 says a save is atomic and all-or-nothing, and that a save which is
//! interrupted leaves the previous file intact. This is the protocol that
//! makes that true rather than hoped for:
//!
//! 1. encode the project into bytes, in memory;
//! 2. write those bytes to the scratch slot;
//! 3. read the scratch slot back and compare it, byte for byte, with what was
//!    written;
//! 4. only then commit, which is the one step that changes the project slot.
//!
//! Steps one to three cannot touch the project slot at all, and step four is
//! the seam's single atomic operation. A failure at any point leaves the last
//! good project exactly where it was, and the test suite proves it by failing
//! at each step in turn.

use alloc::vec;
use alloc::vec::Vec;

use sapstudio_abi::seam::{Slot, Storage};
use sapstudio_core::Digest;
use sapstudio_model::Project;

use crate::format;
use crate::status::{IoStatus, Result};

/// Write a project, and return the digest of the file that was committed.
///
/// # Errors
///
/// [`IoStatus::Seam`] for anything storage refuses,
/// [`IoStatus::WriteNotVerified`] if the bytes did not read back as
/// themselves, or an encoding refusal. In every case the project slot is
/// unchanged.
pub fn save(project: &Project, storage: &mut dyn Storage) -> Result<Digest> {
    let file = format::encode(project)?;
    storage.write(Slot::Scratch, &file)?;

    // Read it back before committing. A storage that accepted the write and
    // stored something else is exactly the failure this step exists to catch,
    // and it is cheap next to losing a day's work.
    let mut echoed = vec![0_u8; file.len()];
    let read = storage.read(Slot::Scratch, &mut echoed)?;
    if read != file.len() || echoed != file {
        return Err(IoStatus::WriteNotVerified);
    }

    storage.commit()?;
    Ok(Digest::of(&file))
}

/// Read the committed project.
///
/// # Errors
///
/// [`IoStatus::Seam`] if there is nothing to read, or any decoding refusal.
pub fn load(storage: &dyn Storage) -> Result<Project> {
    let length = storage.len(Slot::Project)?;
    let mut file = Vec::new();
    file.try_reserve(length)
        .map_err(|_| IoStatus::OutOfMemory)?;
    file.resize(length, 0);
    let read = storage.read(Slot::Project, &mut file)?;
    if read != length {
        return Err(IoStatus::TruncatedPayload);
    }
    format::decode(&file)
}
