// SPDX-License-Identifier: GPL-3.0-only
//! Bounded reader for the build-packed First Light UI font.
//!
//! Development tools parse Spleen's BDF source. The kernel sees only SUF1: a
//! fixed 24-byte header followed by consecutive, tightly packed glyph rows.

const HEADER_SIZE: usize = 24;
const MAGIC: [u8; 4] = *b"SUF1";
const VERSION: u8 = 1;
const MAX_WIDTH: u32 = 16;
const MAX_HEIGHT: u32 = 32;
const MAX_ADVANCE: u32 = 32;

/// Every conclusion the SUF1 parser and renderer can report.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    /// The asset or draw request is valid.
    Ok = 0,
    /// A null pointer crossed the C ABI.
    NullArgument = 1,
    /// The fixed header is short, has the wrong magic, or contradicts its body.
    MalformedHeader = 2,
    /// The asset names a PUF version this kernel does not implement.
    UnsupportedVersion = 3,
    /// Cell, baseline, advance, range, or row metrics are invalid.
    BadMetrics = 4,
    /// The requested code point is not in the packed range.
    MissingGlyph = 5,
    /// The last declared glyph is not fully present.
    TruncatedBitmap = 6,
    /// Checked range, glyph, or body arithmetic overflowed.
    SizeOverflow = 7,
    /// The caller did not offer enough bytes for one glyph.
    BufferTooSmall = 8,
    /// The requested destination and clip cannot contain a glyph.
    DestinationClippingFailure = 9,
}

/// Validated SUF1 metrics, copied across the C boundary by value.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Geometry {
    /// Cell bitmap width in pixels.
    pub width: u32,
    /// Cell bitmap height in pixels.
    pub height: u32,
    /// Pixels above the baseline.
    pub ascent: u32,
    /// Pixels below the baseline.
    pub descent: u32,
    /// Horizontal pen advance in pixels.
    pub advance: u32,
    /// Packed bytes in one bitmap row.
    pub row_bytes: u32,
    /// First covered Unicode scalar value.
    pub first: u32,
    /// Number of consecutive covered glyphs.
    pub count: u32,
    /// Declared bytes after the fixed header.
    pub data_length: u32,
}

fn read_u32(blob: &[u8], offset: usize) -> Result<u32, Status> {
    let bytes = blob
        .get(offset..offset + 4)
        .ok_or(Status::MalformedHeader)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// Validate the complete SUF1 asset before returning its metrics.
pub fn geometry(blob: &[u8]) -> Result<Geometry, Status> {
    if blob.len() < HEADER_SIZE || blob[0..4] != MAGIC || blob[5] != HEADER_SIZE as u8 {
        return Err(Status::MalformedHeader);
    }
    if blob[4] != VERSION {
        return Err(Status::UnsupportedVersion);
    }

    let width = u32::from(blob[6]);
    let height = u32::from(blob[7]);
    let ascent = u32::from(blob[8]);
    let descent = u32::from(blob[9]);
    let advance = u32::from(blob[10]);
    let row_bytes = u32::from(blob[11]);
    let first = read_u32(blob, 12)?;
    let count = read_u32(blob, 16)?;
    let data_length = read_u32(blob, 20)?;

    let expected_rows = match width.checked_add(7) {
        Some(bits) => bits / 8,
        None => return Err(Status::SizeOverflow),
    };
    let vertical = match ascent.checked_add(descent) {
        Some(value) => value,
        None => return Err(Status::SizeOverflow),
    };
    if width == 0
        || width > MAX_WIDTH
        || height == 0
        || height > MAX_HEIGHT
        || vertical != height
        || ascent == 0
        || advance < width
        || advance > MAX_ADVANCE
        || row_bytes == 0
        || row_bytes != expected_rows
        || count == 0
    {
        return Err(Status::BadMetrics);
    }
    if first.checked_add(count).is_none() {
        return Err(Status::SizeOverflow);
    }

    let glyph_bytes = height
        .checked_mul(row_bytes)
        .ok_or(Status::SizeOverflow)?;
    let expected_data = count
        .checked_mul(glyph_bytes)
        .ok_or(Status::SizeOverflow)?;
    if data_length != expected_data {
        return Err(Status::MalformedHeader);
    }
    let total = HEADER_SIZE
        .checked_add(data_length as usize)
        .ok_or(Status::SizeOverflow)?;
    if blob.len() < total {
        return Err(Status::TruncatedBitmap);
    }
    if blob.len() != total {
        return Err(Status::MalformedHeader);
    }

    Ok(Geometry {
        width,
        height,
        ascent,
        descent,
        advance,
        row_bytes,
        first,
        count,
        data_length,
    })
}

/// Copy one packed glyph, returning the bytes written.
pub fn glyph(blob: &[u8], code: u32, out: &mut [u8]) -> Result<usize, Status> {
    let metrics = geometry(blob)?;
    let end = metrics
        .first
        .checked_add(metrics.count)
        .ok_or(Status::SizeOverflow)?;
    if code < metrics.first || code >= end {
        return Err(Status::MissingGlyph);
    }
    let glyph_bytes = metrics
        .height
        .checked_mul(metrics.row_bytes)
        .ok_or(Status::SizeOverflow)? as usize;
    if out.len() < glyph_bytes {
        return Err(Status::BufferTooSmall);
    }
    let index = (code - metrics.first) as usize;
    let relative = index
        .checked_mul(glyph_bytes)
        .ok_or(Status::SizeOverflow)?;
    let start = HEADER_SIZE
        .checked_add(relative)
        .ok_or(Status::SizeOverflow)?;
    let end = start
        .checked_add(glyph_bytes)
        .ok_or(Status::SizeOverflow)?;
    let bitmap = blob.get(start..end).ok_or(Status::TruncatedBitmap)?;
    out[..glyph_bytes].copy_from_slice(bitmap);
    Ok(glyph_bytes)
}

/// Stable FNV-1a fingerprint of the exact packed font bytes.
pub fn fingerprint(blob: &[u8]) -> u64 {
    let mut value = 0xcbf29ce484222325u64;
    for byte in blob {
        value ^= u64::from(*byte);
        value = value.wrapping_mul(0x100000001b3);
    }
    value
}

fn put_u32(buffer: &mut [u8], offset: usize, value: u32) {
    buffer[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn fixture(buffer: &mut [u8; 32]) -> usize {
    buffer[0..4].copy_from_slice(&MAGIC);
    buffer[4] = VERSION;
    buffer[5] = HEADER_SIZE as u8;
    buffer[6] = 9;
    buffer[7] = 2;
    buffer[8] = 1;
    buffer[9] = 1;
    buffer[10] = 9;
    buffer[11] = 2;
    put_u32(buffer, 12, 0x20);
    put_u32(buffer, 16, 1);
    put_u32(buffer, 20, 4);
    buffer[24..28].copy_from_slice(&[0xAA, 0x80, 0x55, 0x00]);
    28
}

/// Exercise every parser refusal using one-field synthetic corruptions.
pub fn self_test() -> bool {
    let mut buffer = [0u8; 32];
    let used = fixture(&mut buffer);
    let good = match geometry(&buffer[..used]) {
        Ok(value) => value,
        Err(_) => return false,
    };
    if good.width != 9 || good.height != 2 || good.data_length != 4 {
        return false;
    }

    let mut bitmap = [0u8; 4];
    if glyph(&buffer[..used], 0x20, &mut bitmap) != Ok(4)
        || bitmap != [0xAA, 0x80, 0x55, 0x00]
    {
        return false;
    }
    if geometry(&buffer[..HEADER_SIZE - 1]) != Err(Status::MalformedHeader) {
        return false;
    }
    let mut damaged = buffer;
    damaged[0] = b'X';
    if geometry(&damaged[..used]) != Err(Status::MalformedHeader) {
        return false;
    }
    let mut damaged = buffer;
    damaged[4] = VERSION + 1;
    if geometry(&damaged[..used]) != Err(Status::UnsupportedVersion) {
        return false;
    }
    let mut damaged = buffer;
    damaged[8] = 2;
    if geometry(&damaged[..used]) != Err(Status::BadMetrics) {
        return false;
    }
    if glyph(&buffer[..used], 0x21, &mut bitmap) != Err(Status::MissingGlyph) {
        return false;
    }
    if geometry(&buffer[..used - 1]) != Err(Status::TruncatedBitmap) {
        return false;
    }
    let mut damaged = buffer;
    put_u32(&mut damaged, 12, u32::MAX);
    put_u32(&mut damaged, 16, 2);
    if geometry(&damaged[..used]) != Err(Status::SizeOverflow) {
        return false;
    }
    let mut cramped = [0u8; 3];
    if glyph(&buffer[..used], 0x20, &mut cramped) != Err(Status::BufferTooSmall) {
        return false;
    }
    fingerprint(&buffer[..used]) == fingerprint(&buffer[..used])
        && fingerprint(&buffer[..used]) != fingerprint(&buffer[..used - 1])
}
