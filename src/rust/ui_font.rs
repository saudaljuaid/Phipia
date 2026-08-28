// SPDX-License-Identifier: GPL-3.0-only
//! Bounded reader for the build-packed antialiased Inter UI font.
//!
//! Development tools rasterize the pinned TrueType source. The kernel sees
//! only SUF2: fixed geometry followed by one advance byte and one 8-bit alpha
//! bitmap per printable ASCII glyph.

const HEADER_SIZE: usize = 24;
const MAGIC: [u8; 4] = *b"SUF2";
const VERSION: u8 = 2;
const MAX_WIDTH: u32 = 20;
const MAX_HEIGHT: u32 = 32;
const MAX_ADVANCE: u32 = 32;

#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Stable status values exposed through the C UI-font ABI.
pub enum Status {
    /// The operation completed successfully.
    Ok = 0,
    /// A required caller pointer was null.
    NullArgument = 1,
    /// The SUF2 magic, header layout, length, or payload was malformed.
    MalformedHeader = 2,
    /// The packed font used an unsupported format version.
    UnsupportedVersion = 3,
    /// Font dimensions, vertical metrics, row bytes, or advance were invalid.
    BadMetrics = 4,
    /// The requested code point was outside printable ASCII.
    MissingGlyph = 5,
    /// A glyph bitmap ended before its declared length.
    TruncatedBitmap = 6,
    /// Checked metric or offset arithmetic overflowed.
    SizeOverflow = 7,
    /// The caller's glyph buffer was too small.
    BufferTooSmall = 8,
    /// The C renderer rejected a destination or clipping contract.
    DestinationClippingFailure = 9,
}

#[inline(never)]
fn copy_byte(destination: &mut u8, source: &u8) {
    *destination = *source;
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Checked SUF2 geometry shared with the C renderer.
pub struct Geometry {
    /// Fixed alpha-cell width in pixels.
    pub width: u32,
    /// Fixed alpha-cell height in pixels.
    pub height: u32,
    /// Pixels from glyph top to the text baseline.
    pub ascent: u32,
    /// Pixels reserved below the text baseline.
    pub descent: u32,
    /// Maximum declared advance. Individual glyphs carry their own value.
    pub advance: u32,
    /// Alpha bytes per row; SUF2 stores one byte per source pixel.
    pub row_bytes: u32,
    /// First represented Unicode scalar value.
    pub first: u32,
    /// Number of consecutive represented code points.
    pub count: u32,
    /// Exact glyph payload length after the fixed header.
    pub data_length: u32,
}

fn read_u32(blob: &[u8], offset: usize) -> Result<u32, Status> {
    let bytes = blob
        .get(offset..offset + 4)
        .ok_or(Status::MalformedHeader)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// Validate a complete SUF2 font and return its checked metrics.
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
    let vertical = ascent.checked_add(descent).ok_or(Status::SizeOverflow)?;
    if width == 0
        || width > MAX_WIDTH
        || height == 0
        || height > MAX_HEIGHT
        || vertical != height
        || ascent == 0
        || advance == 0
        || advance > MAX_ADVANCE
        || row_bytes != width
        || count == 0
    {
        return Err(Status::BadMetrics);
    }
    first.checked_add(count).ok_or(Status::SizeOverflow)?;
    let bitmap_bytes = height.checked_mul(row_bytes).ok_or(Status::SizeOverflow)?;
    let glyph_bytes = bitmap_bytes.checked_add(1).ok_or(Status::SizeOverflow)?;
    let expected_data = count.checked_mul(glyph_bytes).ok_or(Status::SizeOverflow)?;
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

fn glyph_start(blob: &[u8], code: u32) -> Result<(Geometry, usize), Status> {
    let metrics = geometry(blob)?;
    let end = metrics.first.checked_add(metrics.count).ok_or(Status::SizeOverflow)?;
    if code < metrics.first || code >= end {
        return Err(Status::MissingGlyph);
    }
    let bitmap_bytes = metrics
        .height
        .checked_mul(metrics.row_bytes)
        .ok_or(Status::SizeOverflow)? as usize;
    let stride = bitmap_bytes.checked_add(1).ok_or(Status::SizeOverflow)?;
    let relative = (code - metrics.first) as usize;
    let start = HEADER_SIZE
        .checked_add(relative.checked_mul(stride).ok_or(Status::SizeOverflow)?)
        .ok_or(Status::SizeOverflow)?;
    Ok((metrics, start))
}

/// Return the checked proportional advance for one represented glyph.
pub fn advance(blob: &[u8], code: u32) -> Result<u32, Status> {
    let (metrics, start) = glyph_start(blob, code)?;
    let value = u32::from(*blob.get(start).ok_or(Status::TruncatedBitmap)?);
    if value == 0 || value > metrics.advance {
        return Err(Status::BadMetrics);
    }
    Ok(value)
}

/// Copy one represented glyph's fixed-size alpha bitmap.
pub fn glyph(blob: &[u8], code: u32, out: &mut [u8]) -> Result<usize, Status> {
    let (metrics, start) = glyph_start(blob, code)?;
    let bitmap_bytes = metrics
        .height
        .checked_mul(metrics.row_bytes)
        .ok_or(Status::SizeOverflow)? as usize;
    if out.len() < bitmap_bytes {
        return Err(Status::BufferTooSmall);
    }
    let bitmap_start = start.checked_add(1).ok_or(Status::SizeOverflow)?;
    let end = bitmap_start
        .checked_add(bitmap_bytes)
        .ok_or(Status::SizeOverflow)?;
    let bitmap = blob.get(bitmap_start..end).ok_or(Status::TruncatedBitmap)?;
    for index in 0..bitmap_bytes {
        copy_byte(&mut out[index], &bitmap[index]);
    }
    Ok(bitmap_bytes)
}

/// Compute the stable FNV-1a fingerprint used by the C installation proof.
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
    buffer[6] = 2;
    buffer[7] = 2;
    buffer[8] = 1;
    buffer[9] = 1;
    buffer[10] = 2;
    buffer[11] = 2;
    put_u32(buffer, 12, 0x20);
    put_u32(buffer, 16, 1);
    put_u32(buffer, 20, 5);
    buffer[24..29].copy_from_slice(&[2, 0, 127, 255, 64]);
    29
}

/// Exercise valid and malformed synthetic SUF2 inputs.
pub fn self_test() -> bool {
    let mut buffer = [0u8; 32];
    let used = fixture(&mut buffer);
    let good = match geometry(&buffer[..used]) {
        Ok(value) => value,
        Err(_) => return false,
    };
    if good.width != 2 || good.height != 2 || good.data_length != 5 {
        return false;
    }
    let mut bitmap = [0u8; 4];
    if glyph(&buffer[..used], 0x20, &mut bitmap) != Ok(4)
        || bitmap != [0, 127, 255, 64]
        || advance(&buffer[..used], 0x20) != Ok(2)
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
    damaged[11] = 1;
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
    let mut damaged = buffer;
    damaged[24] = 3;
    advance(&damaged[..used], 0x20) == Err(Status::BadMetrics)
        && fingerprint(&buffer[..used]) == fingerprint(&buffer[..used])
        && fingerprint(&buffer[..used]) != fingerprint(&buffer[..used - 1])
}
