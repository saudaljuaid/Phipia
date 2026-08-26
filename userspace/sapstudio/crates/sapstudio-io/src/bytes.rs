// SPDX-License-Identifier: GPL-3.0-only
//! Reading and writing fixed-width fields.
//!
//! Every read is checked against what is left (R-11.2), and every write grows
//! its buffer fallibly and refuses past a bound (R-5.1, R-5.2). These two
//! types are the only way this crate touches bytes, so "the parser forgot a
//! bounds check" is not a thing that can be true of it.

use alloc::vec::Vec;

use crate::status::{IoStatus, Result};

/// A cursor over bytes that refuses to run off the end.
pub struct Reader<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> Reader<'a> {
    /// Read from the start of a slice.
    #[must_use]
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }

    /// How many bytes have not been read.
    #[must_use]
    pub const fn remaining(&self) -> usize {
        self.bytes.len() - self.position
    }

    /// Whether every byte has been read.
    #[must_use]
    pub const fn is_finished(&self) -> bool {
        self.position == self.bytes.len()
    }

    /// Take a fixed number of bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`] if that many are not left.
    pub fn take(&mut self, count: usize) -> Result<&'a [u8]> {
        let end = self
            .position
            .checked_add(count)
            .ok_or(IoStatus::TruncatedField)?;
        let slice = self
            .bytes
            .get(self.position..end)
            .ok_or(IoStatus::TruncatedField)?;
        self.position = end;
        Ok(slice)
    }

    /// Take one byte.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }

    /// Take a little-endian `u16`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u16(&mut self) -> Result<u16> {
        let mut bytes = [0; 2];
        bytes.copy_from_slice(self.take(2)?);
        Ok(u16::from_le_bytes(bytes))
    }

    /// Take a little-endian `u32`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u32(&mut self) -> Result<u32> {
        let mut bytes = [0; 4];
        bytes.copy_from_slice(self.take(4)?);
        Ok(u32::from_le_bytes(bytes))
    }

    /// Take a little-endian `u64`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u64(&mut self) -> Result<u64> {
        let mut bytes = [0; 8];
        bytes.copy_from_slice(self.take(8)?);
        Ok(u64::from_le_bytes(bytes))
    }

    /// Take a little-endian `i64`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn i64(&mut self) -> Result<i64> {
        let mut bytes = [0; 8];
        bytes.copy_from_slice(self.take(8)?);
        Ok(i64::from_le_bytes(bytes))
    }

    /// Take a little-endian `i32`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn i32(&mut self) -> Result<i32> {
        let mut bytes = [0; 4];
        bytes.copy_from_slice(self.take(4)?);
        Ok(i32::from_le_bytes(bytes))
    }

    /// Take thirty-two bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn digest_bytes(&mut self) -> Result<[u8; 32]> {
        let mut bytes = [0; 32];
        bytes.copy_from_slice(self.take(32)?);
        Ok(bytes)
    }
}

/// A growing buffer that refuses past a bound.
pub struct Writer {
    bytes: Vec<u8>,
    limit: usize,
}

impl Writer {
    /// A buffer that will refuse to exceed `limit` bytes.
    #[must_use]
    pub const fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
        }
    }

    /// How many bytes have been written.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether nothing has been written.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// The bytes.
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.bytes
    }

    /// Take the bytes.
    #[must_use]
    pub fn finish(self) -> Vec<u8> {
        self.bytes
    }

    /// Append bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::PayloadTooLarge`] past the bound, or
    /// [`IoStatus::OutOfMemory`].
    pub fn bytes(&mut self, bytes: &[u8]) -> Result<()> {
        let end = self
            .bytes
            .len()
            .checked_add(bytes.len())
            .ok_or(IoStatus::PayloadTooLarge)?;
        if end > self.limit {
            return Err(IoStatus::PayloadTooLarge);
        }
        self.bytes
            .try_reserve(bytes.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        self.bytes.extend_from_slice(bytes);
        Ok(())
    }

    /// Append one byte.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u8(&mut self, value: u8) -> Result<()> {
        self.bytes(&[value])
    }

    /// Append a little-endian `u16`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u16(&mut self, value: u16) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `u32`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u32(&mut self, value: u32) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `u64`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u64(&mut self, value: u64) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `i32`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn i32(&mut self, value: i32) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `i64`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn i64(&mut self, value: i64) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }
}
