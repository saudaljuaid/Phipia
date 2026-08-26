// SPDX-License-Identifier: GPL-3.0-only
//! Storage in memory, with faults on demand.
//!
//! Every seam has two implementations: the Sapote one, and a deterministic one
//! the host suite drives. This is the second. It exists mostly so that R-9.4's
//! negative control is a test rather than a thought experiment: it can be told
//! to fail at each step of a save, and the project slot must survive all of
//! them.

use alloc::vec::Vec;

use sapstudio_abi::seam::{Result, SeamStatus, Slot, Storage};

/// Where a fault should happen.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Fault {
    /// Nothing goes wrong.
    #[default]
    None,
    /// The write to the scratch slot is refused.
    OnWrite,
    /// The write is accepted and stores something else, which the read-back
    /// step must catch.
    Corrupting,
    /// Reading the scratch slot back is refused.
    OnReadBack,
    /// The commit is refused, after everything else succeeded.
    OnCommit,
}

/// Two extents and a swap, in memory.
#[derive(Clone, Debug)]
pub struct MemoryStorage {
    project: Option<Vec<u8>>,
    scratch: Option<Vec<u8>>,
    capacity: usize,
    fault: Fault,
    reads: usize,
    commits: usize,
}

impl MemoryStorage {
    /// Empty storage with a per-slot capacity.
    #[must_use]
    pub const fn new(capacity: usize) -> Self {
        Self {
            project: None,
            scratch: None,
            capacity,
            fault: Fault::None,
            reads: 0,
            commits: 0,
        }
    }

    /// Arrange for the next save to fail in a particular way.
    pub fn set_fault(&mut self, fault: Fault) {
        self.fault = fault;
    }

    /// What the project slot holds, if anything.
    #[must_use]
    pub fn committed(&self) -> Option<&[u8]> {
        self.project.as_deref()
    }

    /// How many times a slot has been read.
    #[must_use]
    pub const fn reads(&self) -> usize {
        self.reads
    }

    /// How many commits have succeeded.
    #[must_use]
    pub const fn commits(&self) -> usize {
        self.commits
    }

    fn slot(&self, slot: Slot) -> Option<&Vec<u8>> {
        match slot {
            Slot::Project => self.project.as_ref(),
            Slot::Scratch => self.scratch.as_ref(),
        }
    }
}

impl Storage for MemoryStorage {
    fn capacity(&self, _slot: Slot) -> usize {
        self.capacity
    }

    fn len(&self, slot: Slot) -> Result<usize> {
        self.slot(slot)
            .map_or(Err(SeamStatus::Empty), |bytes| Ok(bytes.len()))
    }

    fn read(&self, slot: Slot, into: &mut [u8]) -> Result<usize> {
        if self.fault == Fault::OnReadBack && slot == Slot::Scratch {
            return Err(SeamStatus::Refused);
        }
        let stored = self.slot(slot).ok_or(SeamStatus::Empty)?;
        if into.len() < stored.len() {
            return Err(SeamStatus::TooLarge);
        }
        into[..stored.len()].copy_from_slice(stored);
        Ok(stored.len())
    }

    fn write(&mut self, slot: Slot, bytes: &[u8]) -> Result<()> {
        if self.fault == Fault::OnWrite {
            return Err(SeamStatus::Refused);
        }
        if bytes.len() > self.capacity {
            return Err(SeamStatus::TooLarge);
        }
        let mut stored = Vec::new();
        stored
            .try_reserve(bytes.len())
            .map_err(|_| SeamStatus::Refused)?;
        stored.extend_from_slice(bytes);
        if self.fault == Fault::Corrupting {
            // One byte, in the middle, exactly as a bad sector would.
            if let Some(byte) = stored.get_mut(bytes.len() / 2) {
                *byte ^= 0x01;
            }
        }
        match slot {
            Slot::Project => self.project = Some(stored),
            Slot::Scratch => self.scratch = Some(stored),
        }
        Ok(())
    }

    fn commit(&mut self) -> Result<()> {
        if self.fault == Fault::OnCommit {
            return Err(SeamStatus::Refused);
        }
        let staged = self.scratch.take().ok_or(SeamStatus::Empty)?;
        self.project = Some(staged);
        self.commits += 1;
        Ok(())
    }
}
