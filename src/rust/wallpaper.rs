// SPDX-License-Identifier: GPL-3.0-only
//! Bounded decoding for the SPW3 RGB565 photographic wallpaper collection.

const HEADER_SIZE: usize = 12;
const PIXEL_BITS: usize = 16;
const BYTES_PER_PIXEL: usize = 2;
const MAGIC: [u8; 4] = *b"SPW3";
const SOURCE_WIDTH: u32 = 1024;
const SOURCE_HEIGHT: u32 = 768;
const MAX_FRAMES: u32 = 32;
const MAX_OUTPUT_WIDTH: u32 = 1920;
const MAX_OUTPUT_HEIGHT: u32 = 1200;

#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Stable status values exposed through the C wallpaper ABI.
pub enum Status {
    /// The operation completed successfully.
    Ok = 0,
    /// A required caller pointer was null.
    NullArgument = 1,
    /// The album header, body length, or magic was invalid.
    BadHeader = 2,
    /// The source dimensions or frame count exceeded the contract.
    BadGeometry = 3,
    /// The album did not declare the exact RGB565 pixel layout.
    BadPalette = 4,
    /// The caller's output slice was shorter than the requested image.
    BufferTooSmall = 5,
    /// The requested frame index was outside the album.
    BadFrame = 6,
    /// The requested output dimensions were zero or exceeded the bound.
    BadOutput = 7,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
/// Checked source geometry for one SPW3 album.
pub struct Geometry {
    /// Source-frame width in pixels.
    pub width: u32,
    /// Source-frame height in pixels.
    pub height: u32,
    /// Number of complete frames in the album.
    pub frames: u32,
}

#[derive(Clone, Copy)]
/// Destination framebuffer channel placement.
pub struct Format {
    /// Bit shift for the red channel.
    pub red_shift: u8,
    /// Bit shift for the green channel.
    pub green_shift: u8,
    /// Bit shift for the blue channel.
    pub blue_shift: u8,
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, Status> {
    let low = *bytes.get(offset).ok_or(Status::BadHeader)?;
    let high_offset = offset.checked_add(1).ok_or(Status::BadHeader)?;
    let high = *bytes.get(high_offset).ok_or(Status::BadHeader)?;
    Ok(u16::from(low) | (u16::from(high) << 8))
}

fn rgb565(pixels: &[u8], index: usize) -> Result<(u32, u32, u32), Status> {
    let offset = index
        .checked_mul(BYTES_PER_PIXEL)
        .ok_or(Status::BadHeader)?;
    let packed = read_u16(pixels, offset)?;
    let red = u32::from((packed >> 11) & 0x1f) * 255 / 31;
    let green = u32::from((packed >> 5) & 0x3f) * 255 / 63;
    let blue = u32::from(packed & 0x1f) * 255 / 31;
    Ok((red, green, blue))
}

/// Validate an SPW3 album and return its exact source geometry.
pub fn geometry(blob: &[u8]) -> Result<Geometry, Status> {
    if blob.get(..HEADER_SIZE).is_none() || blob.get(..MAGIC.len()) != Some(MAGIC.as_slice()) {
        return Err(Status::BadHeader);
    }
    let width = u32::from(read_u16(blob, 4)?);
    let height = u32::from(read_u16(blob, 6)?);
    let pixel_bits = usize::from(read_u16(blob, 8)?);
    let frames = u32::from(read_u16(blob, 10)?);
    if width != SOURCE_WIDTH || height != SOURCE_HEIGHT || frames == 0 || frames > MAX_FRAMES {
        return Err(Status::BadGeometry);
    }
    if pixel_bits != PIXEL_BITS {
        return Err(Status::BadPalette);
    }
    let frame_pixels = (width as usize)
        .checked_mul(height as usize)
        .ok_or(Status::BadHeader)?;
    let body = frame_pixels
        .checked_mul(BYTES_PER_PIXEL)
        .and_then(|value| value.checked_mul(frames as usize))
        .ok_or(Status::BadHeader)?;
    let expected = HEADER_SIZE.checked_add(body).ok_or(Status::BadHeader)?;
    if blob.len() != expected {
        return Err(Status::BadHeader);
    }
    Ok(Geometry {
        width,
        height,
        frames,
    })
}

/// Decode one checked frame into a bounded caller-selected output size.
pub fn decode(
    blob: &[u8],
    frame: u32,
    out: &mut [u32],
    out_width: u32,
    out_height: u32,
    format: &Format,
) -> Result<Geometry, Status> {
    let geometry = geometry(blob)?;
    if frame >= geometry.frames {
        return Err(Status::BadFrame);
    }
    if out_width == 0
        || out_height == 0
        || out_width > MAX_OUTPUT_WIDTH
        || out_height > MAX_OUTPUT_HEIGHT
    {
        return Err(Status::BadOutput);
    }
    let count = (out_width as usize)
        .checked_mul(out_height as usize)
        .ok_or(Status::BadOutput)?;
    if out.len() < count {
        return Err(Status::BufferTooSmall);
    }
    let frame_pixels = (geometry.width as usize) * (geometry.height as usize);
    let frame_bytes = frame_pixels
        .checked_mul(BYTES_PER_PIXEL)
        .ok_or(Status::BadHeader)?;
    let frame_offset = (frame as usize)
        .checked_mul(frame_bytes)
        .and_then(|value| value.checked_add(HEADER_SIZE))
        .ok_or(Status::BadHeader)?;
    let frame_end = frame_offset
        .checked_add(frame_bytes)
        .ok_or(Status::BadHeader)?;
    let pixels = blob.get(frame_offset..frame_end).ok_or(Status::BadHeader)?;
    if out_width == geometry.width && out_height == geometry.height {
        for (index, output) in out.iter_mut().take(frame_pixels).enumerate() {
            let (red, green, blue) = rgb565(pixels, index)?;
            *output = (red << format.red_shift)
                | (green << format.green_shift)
                | (blue << format.blue_shift);
        }
        return Ok(geometry);
    }
    let sample = |x: usize, y: usize| -> Result<(u32, u32, u32), Status> {
        let index = y
            .checked_mul(geometry.width as usize)
            .and_then(|value| value.checked_add(x))
            .ok_or(Status::BadHeader)?;
        rgb565(pixels, index)
    };
    let blend = |first: u32, second: u32, fraction: u64, denominator: u64| -> u32 {
        ((u64::from(first) * (denominator - fraction)
            + u64::from(second) * fraction
            + denominator / 2)
            / denominator) as u32
    };
    for y in 0..out_height {
        let denominator_y = u64::from(out_height.saturating_sub(1).max(1));
        let scaled_y = u64::from(y) * u64::from(geometry.height - 1);
        let source_y = (scaled_y / denominator_y) as usize;
        let next_y = (source_y + 1).min(geometry.height as usize - 1);
        let fraction_y = scaled_y % denominator_y;
        for x in 0..out_width {
            let denominator_x = u64::from(out_width.saturating_sub(1).max(1));
            let scaled_x = u64::from(x) * u64::from(geometry.width - 1);
            let source_x = (scaled_x / denominator_x) as usize;
            let next_x = (source_x + 1).min(geometry.width as usize - 1);
            let fraction_x = scaled_x % denominator_x;
            let top_left = sample(source_x, source_y)?;
            let top_right = sample(next_x, source_y)?;
            let bottom_left = sample(source_x, next_y)?;
            let bottom_right = sample(next_x, next_y)?;
            let channel =
                |left_top: u32, right_top: u32, left_bottom: u32, right_bottom: u32| -> u32 {
                    let top = blend(left_top, right_top, fraction_x, denominator_x);
                    let bottom = blend(left_bottom, right_bottom, fraction_x, denominator_x);
                    blend(top, bottom, fraction_y, denominator_y)
                };
            let red = channel(top_left.0, top_right.0, bottom_left.0, bottom_right.0);
            let green = channel(top_left.1, top_right.1, bottom_left.1, bottom_right.1);
            let blue = channel(top_left.2, top_right.2, bottom_left.2, bottom_right.2);
            let output_index = (y as usize)
                .checked_mul(out_width as usize)
                .and_then(|value| value.checked_add(x as usize))
                .ok_or(Status::BadOutput)?;
            let output = out.get_mut(output_index).ok_or(Status::BufferTooSmall)?;
            *output = (red << format.red_shift)
                | (green << format.green_shift)
                | (blue << format.blue_shift);
        }
    }
    Ok(geometry)
}

/// Exercise the production album and representative malformed inputs.
pub fn self_test(blob: &[u8]) -> bool {
    let production = match geometry(blob) {
        Ok(value) => value,
        Err(_) => return false,
    };
    if production
        != (Geometry {
            width: SOURCE_WIDTH,
            height: SOURCE_HEIGHT,
            frames: 14,
        })
    {
        return false;
    }
    let mut good = [0u8; HEADER_SIZE + 4];
    good[..4].copy_from_slice(&MAGIC);
    good[4..6].copy_from_slice(&SOURCE_WIDTH.to_le_bytes()[..2]);
    good[6..8].copy_from_slice(&SOURCE_HEIGHT.to_le_bytes()[..2]);
    good[8..10].copy_from_slice(&(PIXEL_BITS as u16).to_le_bytes());
    good[10..12].copy_from_slice(&1u16.to_le_bytes());
    // The synthetic is intentionally short; header refusals must happen first.
    geometry(&good) == Err(Status::BadHeader)
        && geometry(&good[..11]) == Err(Status::BadHeader)
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
            good[4..6].copy_from_slice(&(SOURCE_WIDTH as u16).to_le_bytes());
            good[8..10].copy_from_slice(&8u16.to_le_bytes());
            geometry(&good) == Err(Status::BadPalette)
        }
}
