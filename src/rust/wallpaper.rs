// SPDX-License-Identifier: GPL-3.0-only
//! Bounded decoding for the deterministic SPW1 desktop wallpaper.

const HEADER_SIZE: usize = 10;
const PALETTE_ENTRIES: usize = 256;
const PALETTE_BYTES: usize = PALETTE_ENTRIES * 3;
const MAGIC: [u8; 4] = *b"SPW1";

/// Result codes mirrored by `include/sapote/wallpaper.h`.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    /// The complete asset decoded.
    Ok = 0,
    /// A pointer supplied over the C boundary was null.
    NullArgument = 1,
    /// The header or total byte length was invalid.
    BadHeader = 2,
    /// The declared geometry was not the supported 1024 by 768 canvas.
    BadGeometry = 3,
    /// The palette was not exactly 256 RGB entries.
    BadPalette = 4,
    /// The output buffer could not hold the declared pixels.
    BufferTooSmall = 5,
}

/// The dimensions declared by a checked SPW1 asset.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Geometry {
    /// Pixel width.
    pub width: u32,
    /// Pixel height.
    pub height: u32,
}

/// Framebuffer channel placement supplied by the boot loader.
#[derive(Clone, Copy)]
pub struct Format {
    /// Red channel bit position.
    pub red_shift: u8,
    /// Green channel bit position.
    pub green_shift: u8,
    /// Blue channel bit position.
    pub blue_shift: u8,
}

fn read_u16(bytes: &[u8]) -> u16 {
    u16::from(bytes[0]) | (u16::from(bytes[1]) << 8)
}

/// Validate a complete asset and return its geometry.
pub fn geometry(blob: &[u8]) -> Result<Geometry, Status> {
    let header = blob.get(..HEADER_SIZE).ok_or(Status::BadHeader)?;
    if header[..4] != MAGIC {
        return Err(Status::BadHeader);
    }
    let width = u32::from(read_u16(&header[4..6]));
    let height = u32::from(read_u16(&header[6..8]));
    let palette_entries = usize::from(read_u16(&header[8..10]));
    if width != 1024 || height != 768 {
        return Err(Status::BadGeometry);
    }
    if palette_entries != PALETTE_ENTRIES {
        return Err(Status::BadPalette);
    }
    let pixels = (width as usize) * (height as usize);
    if blob.len() != HEADER_SIZE + PALETTE_BYTES + pixels {
        return Err(Status::BadHeader);
    }
    Ok(Geometry { width, height })
}

/// Decode every checked palette index into the framebuffer's packed format.
pub fn decode(blob: &[u8], out: &mut [u32], format: &Format)
    -> Result<Geometry, Status>
{
    let geometry = geometry(blob)?;
    let count = (geometry.width as usize) * (geometry.height as usize);
    if out.len() < count {
        return Err(Status::BufferTooSmall);
    }
    let palette = &blob[HEADER_SIZE..HEADER_SIZE + PALETTE_BYTES];
    let indices = &blob[HEADER_SIZE + PALETTE_BYTES..];
    for (slot, index) in out[..count].iter_mut().zip(indices.iter()) {
        let base = usize::from(*index) * 3;
        *slot = (u32::from(palette[base]) << format.red_shift)
            | (u32::from(palette[base + 1]) << format.green_shift)
            | (u32::from(palette[base + 2]) << format.blue_shift);
    }
    Ok(geometry)
}

/// Exercise the production asset and malformed-header refusals without allocation.
pub fn self_test(blob: &[u8]) -> bool {
    let mut good = [0u8; HEADER_SIZE + PALETTE_BYTES + 4];
    good[..4].copy_from_slice(&MAGIC);
    good[4..6].copy_from_slice(&1024u16.to_le_bytes());
    good[6..8].copy_from_slice(&768u16.to_le_bytes());
    good[8..10].copy_from_slice(&256u16.to_le_bytes());

    geometry(blob) == Ok(Geometry { width: 1024, height: 768 })
        // A tiny synthetic cannot pass the exact-length production geometry;
        // the individual refusals still prove they precede all indexing.
        && geometry(&good) == Err(Status::BadHeader)
        && geometry(&good[..9]) == Err(Status::BadHeader)
        && {
            good[0] = b'X';
            geometry(&good) == Err(Status::BadHeader)
        }
        && {
            good[0] = b'S';
            good[4..6].copy_from_slice(&800u16.to_le_bytes());
            geometry(&good) == Err(Status::BadGeometry)
        }
        && {
            good[4..6].copy_from_slice(&1024u16.to_le_bytes());
            good[8..10].copy_from_slice(&255u16.to_le_bytes());
            geometry(&good) == Err(Status::BadPalette)
        }
}
