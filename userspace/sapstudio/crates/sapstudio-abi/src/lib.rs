// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_op_in_unsafe_fn)]
#![allow(
    clippy::doc_markdown,
    reason = "SapStudio and Sapote are product names, not identifiers"
)]
//! Where SapStudio meets Sapote.
//!
//! This is one of the two crates permitted to contain `unsafe` (R-3.1.4), and
//! it is deliberately the smallest thing that can be a boundary: the raw
//! syscall sequence, the proposed native call numbers, and the five seams
//! everything above it talks through.
//!
//! # Status
//!
//! Sapote v1.1.0 has **no native application ABI**. Its only userspace surface
//! is a measured Linux compatibility boundary that admits three
//! checksum-pinned BusyBox programs and refuses everything else, and widening
//! that boundary to fit an application would destroy the property that makes
//! it trustworthy.
//!
//! So the numbers in [`syscall`] are a *proposal*: they are `SAP-01` in
//! `docs/PLATFORM_CONTRACT.md`, written down here in the shape SapStudio needs
//! so that the kernel work has something exact to implement against. Until
//! Sapote provides them, an image built from this crate is a conforming ELF
//! that the kernel will refuse to run — which is the honest state of affairs,
//! and is why [`seam`] exists: everything above this crate is written against
//! traits, and the host test suite supplies its own implementations of them.

pub mod seam;
pub mod syscall;

pub use seam::{Console, Slot, Storage, Time};
pub use syscall::{ABI_VERSION, AbiVersion};
