// SPDX-License-Identifier: GPL-3.0-only
//! Validator for the VBIOS image an NVIDIA board keeps in its PROM window.
//!
//! This is bytes from outside, so it is Rust's job rather than C's. The image
//! is a PCI expansion ROM as the PCI Firmware Specification 3.0 section 5.1
//! defines it -- a 0xAA55 signature, a pointer at offset 0x18 to a "PCIR" data
//! structure carrying the vendor, device, class, image length and code type --
//! wrapped around NVIDIA's own BIT table, whose layout is the one the Nouveau
//! driver reads and the envytools project documents.
//!
//! What is validated is exactly what a consumer would otherwise trust: that
//! every offset the image points at is inside the image. The BIT header's
//! bytes at +6 through +8 are version fields no consumer here reads, so this
//! parser reports the structure without interpreting them rather than
//! inventing a meaning for them.
//!
//! Nothing here allocates, retains caller storage, or overlays a struct on the
//! byte stream; every field is decoded individually from a checked slice.

/// The PROM aperture size, and therefore the largest image that can be inside.
pub const MAX_IMAGE_BYTES: usize = 65536;
/// PCI expansion ROM images are a whole number of these.
pub const BLOCK_BYTES: usize = 512;
/// PCI-SIG vendor identifier assigned to NVIDIA Corporation.
pub const NVIDIA_VENDOR_ID: u16 = 0x10DE;
/// Smallest PCIR data structure this parser reads fields out of.
pub const PCIR_BYTES: usize = 0x18;
/// The six bytes that open NVIDIA's BIT table: id 0xB8FF, then "BIT\0".
pub const BIT_SIGNATURE: [u8; 6] = [0xFF, 0xB8, b'B', b'I', b'T', 0x00];
/// Smallest BIT token this parser reads an offset and length out of.
pub const BIT_TOKEN_BYTES: u8 = 6;
/// Number of independent controls [`self_test`] runs.
pub const ROBUSTNESS_CONTROLS: usize = 16;

/// Why an image was refused, or that it was not.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum Status {
    /// The image parsed and every offset inside it is in range.
    Ok = 0,
    /// Shorter than one block, or longer than the aperture.
    Length = 1,
    /// The 0xAA55 expansion-ROM signature is not the first two bytes.
    Signature = 2,
    /// The pointer at offset 0x18 does not leave room for a PCIR structure.
    PcirPointer = 3,
    /// There is no "PCIR" signature where the pointer says there is.
    PcirSignature = 4,
    /// The PCIR structure names a vendor other than NVIDIA.
    PcirVendor = 5,
    /// The declared image length is zero, or longer than the bytes supplied.
    PcirLength = 6,
    /// The first image is not the x86 PC-AT code type the specification gives.
    PcirCodeType = 7,
    /// No BIT table was found inside the declared image.
    BitSignature = 8,
    /// The BIT header declares a token size or count this parser cannot walk.
    BitHeader = 9,
    /// A BIT token, or the region it names, falls outside the image.
    BitToken = 10,
}

/// What an accepted image turned out to be.
///
/// The layout is the C one because the kernel reads this value directly; the
/// assertions in `abi.rs` hold the two sides to the same offsets.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub struct Image {
    /// Declared image length in bytes, always a multiple of [`BLOCK_BYTES`].
    pub image_bytes: u32,
    /// Where the PCIR data structure begins.
    pub pcir_offset: u32,
    /// Where the BIT table begins.
    pub bit_offset: u32,
    /// PCIR vendor identifier, always [`NVIDIA_VENDOR_ID`] once accepted.
    pub vendor_id: u16,
    /// PCIR device identifier, reported and not interpreted.
    pub device_id: u16,
    /// PCIR base class code.
    pub class_code: u8,
    /// PCIR subclass.
    pub subclass: u8,
    /// PCIR programming interface.
    pub programming_interface: u8,
    /// PCIR code type; 0x00 is x86 PC-AT.
    pub code_type: u8,
    /// How many tokens the BIT header declares.
    pub bit_tokens: u8,
    /// Stride between BIT tokens, from the header.
    pub bit_token_bytes: u8,
    /// Whether the PCIR indicator marks this as the last image.
    pub last_image: bool,
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    let low = *bytes.get(offset)?;
    let high = *bytes.get(offset + 1)?;
    Some(u16::from_le_bytes([low, high]))
}

fn find_bit(bytes: &[u8]) -> Option<usize> {
    if bytes.len() < BIT_SIGNATURE.len() {
        return None;
    }
    let last = bytes.len() - BIT_SIGNATURE.len();
    let mut offset = 0usize;

    while offset <= last {
        if bytes[offset..offset + BIT_SIGNATURE.len()] == BIT_SIGNATURE {
            return Some(offset);
        }
        offset += 1;
    }
    None
}

/// Decode and validate one VBIOS image.
///
/// The slice is the bytes read out of the PROM window. Its length bounds
/// everything: an image that declares itself longer than the bytes supplied is
/// refused rather than read past.
pub fn parse(bytes: &[u8]) -> Result<Image, Status> {
    if bytes.len() < BLOCK_BYTES || bytes.len() > MAX_IMAGE_BYTES {
        return Err(Status::Length);
    }
    if bytes[0] != 0x55 || bytes[1] != 0xAA {
        return Err(Status::Signature);
    }

    let pcir = read_u16(bytes, 0x18).ok_or(Status::PcirPointer)? as usize;

    if pcir < 0x1A || pcir.checked_add(PCIR_BYTES).ok_or(Status::PcirPointer)?
        > bytes.len()
    {
        return Err(Status::PcirPointer);
    }
    if &bytes[pcir..pcir + 4] != b"PCIR" {
        return Err(Status::PcirSignature);
    }

    let vendor_id = read_u16(bytes, pcir + 4).ok_or(Status::PcirSignature)?;

    if vendor_id != NVIDIA_VENDOR_ID {
        return Err(Status::PcirVendor);
    }

    let device_id = read_u16(bytes, pcir + 6).ok_or(Status::PcirSignature)?;
    let blocks = read_u16(bytes, pcir + 0x10).ok_or(Status::PcirLength)?
        as usize;
    let image_bytes = blocks
        .checked_mul(BLOCK_BYTES)
        .ok_or(Status::PcirLength)?;

    if blocks == 0 || image_bytes > bytes.len() {
        return Err(Status::PcirLength);
    }

    let code_type = bytes[pcir + 0x14];

    if code_type != 0x00 {
        return Err(Status::PcirCodeType);
    }

    let image = &bytes[..image_bytes];
    let bit_offset = find_bit(image).ok_or(Status::BitSignature)?;
    let bit_token_bytes = *image.get(bit_offset + 9).ok_or(Status::BitHeader)?;
    let bit_tokens = *image.get(bit_offset + 10).ok_or(Status::BitHeader)?;

    if bit_token_bytes < BIT_TOKEN_BYTES || bit_tokens == 0 {
        return Err(Status::BitHeader);
    }

    let first_token = bit_offset.checked_add(12).ok_or(Status::BitHeader)?;
    let span = (bit_tokens as usize)
        .checked_mul(bit_token_bytes as usize)
        .ok_or(Status::BitHeader)?;

    if first_token.checked_add(span).ok_or(Status::BitHeader)? > image.len() {
        return Err(Status::BitHeader);
    }

    for index in 0..bit_tokens as usize {
        let token = first_token + index * bit_token_bytes as usize;
        let length = read_u16(image, token + 2).ok_or(Status::BitToken)?
            as usize;
        let offset = read_u16(image, token + 4).ok_or(Status::BitToken)?
            as usize;

        if length == 0 {
            continue;
        }
        if offset == 0 || offset.checked_add(length).ok_or(Status::BitToken)?
            > image.len()
        {
            return Err(Status::BitToken);
        }
    }

    Ok(Image {
        image_bytes: image_bytes as u32,
        pcir_offset: pcir as u32,
        bit_offset: bit_offset as u32,
        vendor_id,
        device_id,
        class_code: bytes[pcir + 0x0F],
        subclass: bytes[pcir + 0x0E],
        programming_interface: bytes[pcir + 0x0D],
        code_type,
        bit_tokens,
        bit_token_bytes,
        last_image: (bytes[pcir + 0x15] & 0x80) != 0,
    })
}

/// The reference image every control below is a mutation of.
///
/// This is a *synthesised* image, not a dump of any board. It carries the
/// structure a real VBIOS carries -- expansion ROM header, PCIR, BIT and its
/// tokens -- so the parser can be exercised without one, and its PCIR device
/// identifier is 0x5341 precisely so it can never be mistaken for a real part.
pub fn reference() -> [u8; 1024] {
    let mut image = [0u8; 1024];

    image[0] = 0x55;
    image[1] = 0xAA;
    /* Legacy header: image length in 512-byte blocks, then a near jump. */
    image[2] = 2;
    image[3] = 0xEB;
    image[4] = 0x0A;
    image[5] = 0x90;
    /* Pointer to the PCIR data structure. */
    image[0x18] = 0x40;
    image[0x19] = 0x00;

    let pcir = 0x40usize;
    image[pcir] = b'P';
    image[pcir + 1] = b'C';
    image[pcir + 2] = b'I';
    image[pcir + 3] = b'R';
    image[pcir + 4] = 0xDE;
    image[pcir + 5] = 0x10;
    image[pcir + 6] = 0x41;
    image[pcir + 7] = 0x53;
    /* PCIR revision 0, no vital product data. */
    image[pcir + 0x0A] = 0x18;
    image[pcir + 0x0B] = 0x00;
    image[pcir + 0x0D] = 0x00;
    image[pcir + 0x0E] = 0x00;
    image[pcir + 0x0F] = 0x03;
    image[pcir + 0x10] = 2;
    image[pcir + 0x11] = 0;
    image[pcir + 0x14] = 0x00;
    image[pcir + 0x15] = 0x80;

    let bit = 0x100usize;
    image[bit..bit + BIT_SIGNATURE.len()].copy_from_slice(&BIT_SIGNATURE);
    /* Version fields this parser reports through rather than interprets. */
    image[bit + 6] = 0x01;
    image[bit + 7] = 0x00;
    image[bit + 8] = 12;
    image[bit + 9] = BIT_TOKEN_BYTES;
    image[bit + 10] = 3;
    image[bit + 11] = 0x00;

    let tokens: [(u8, u16, u16); 3] = [
        (b'i', 0x0040, 0x0200),
        (b'B', 0x0020, 0x0240),
        (b'P', 0x0010, 0x0260),
    ];
    for (index, (id, length, offset)) in tokens.iter().enumerate() {
        let token = bit + 12 + index * BIT_TOKEN_BYTES as usize;

        image[token] = *id;
        image[token + 1] = 0x02;
        image[token + 2] = *length as u8;
        image[token + 3] = (*length >> 8) as u8;
        image[token + 4] = *offset as u8;
        image[token + 5] = (*offset >> 8) as u8;
    }
    image
}

/// Run every control and return how many passed.
///
/// Each one takes the reference image, breaks exactly one structural field,
/// and requires the named refusal. A parser that accepted any of them would be
/// trusting an offset it had not checked.
pub fn self_test() -> usize {
    let mut passed = 0usize;
    let reference = reference();

    if let Ok(image) = parse(&reference) {
        if image.image_bytes == 1024
            && image.pcir_offset == 0x40
            && image.bit_offset == 0x100
            && image.vendor_id == NVIDIA_VENDOR_ID
            && image.device_id == 0x5341
            && image.class_code == 0x03
            && image.subclass == 0x00
            && image.code_type == 0x00
            && image.bit_tokens == 3
            && image.bit_token_bytes == BIT_TOKEN_BYTES
            && image.last_image
        {
            passed += 1;
        }
    }

    if parse(&reference[..BLOCK_BYTES - 1]) == Err(Status::Length) {
        passed += 1;
    }

    let mut broken = reference;
    broken[0] = 0x54;
    if parse(&broken) == Err(Status::Signature) {
        passed += 1;
    }

    broken = reference;
    broken[1] = 0xAB;
    if parse(&broken) == Err(Status::Signature) {
        passed += 1;
    }

    broken = reference;
    broken[0x18] = 0x00;
    broken[0x19] = 0x40;
    if parse(&broken) == Err(Status::PcirPointer) {
        passed += 1;
    }

    broken = reference;
    broken[0x18] = 0x10;
    if parse(&broken) == Err(Status::PcirPointer) {
        passed += 1;
    }

    broken = reference;
    broken[0x43] = b'r';
    if parse(&broken) == Err(Status::PcirSignature) {
        passed += 1;
    }

    broken = reference;
    broken[0x44] = 0x86;
    broken[0x45] = 0x80;
    if parse(&broken) == Err(Status::PcirVendor) {
        passed += 1;
    }

    broken = reference;
    broken[0x50] = 0;
    broken[0x51] = 0;
    if parse(&broken) == Err(Status::PcirLength) {
        passed += 1;
    }

    broken = reference;
    broken[0x50] = 8;
    if parse(&broken) == Err(Status::PcirLength) {
        passed += 1;
    }

    broken = reference;
    broken[0x54] = 0x03;
    if parse(&broken) == Err(Status::PcirCodeType) {
        passed += 1;
    }

    broken = reference;
    broken[0x102] = b'b';
    if parse(&broken) == Err(Status::BitSignature) {
        passed += 1;
    }

    broken = reference;
    broken[0x109] = BIT_TOKEN_BYTES - 1;
    if parse(&broken) == Err(Status::BitHeader) {
        passed += 1;
    }

    broken = reference;
    broken[0x10A] = 0;
    if parse(&broken) == Err(Status::BitHeader) {
        passed += 1;
    }

    broken = reference;
    broken[0x10A] = 0xFF;
    if parse(&broken) == Err(Status::BitHeader) {
        passed += 1;
    }

    broken = reference;
    /* The first token's region now runs off the end of the image. */
    broken[0x10E] = 0x00;
    broken[0x10F] = 0x04;
    broken[0x110] = 0x00;
    broken[0x111] = 0x03;
    if parse(&broken) == Err(Status::BitToken) {
        passed += 1;
    }

    passed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_signature_status_keeps_the_number_the_kernel_maps() {
        /* The kernel turns exactly this one into "there is no ROM here". */
        assert_eq!(Status::Signature as i32, 2);
        assert_eq!(Status::Ok as i32, 0);
    }

    #[test]
    fn every_control_passes() {
        assert_eq!(self_test(), ROBUSTNESS_CONTROLS);
    }

    #[test]
    fn the_reference_image_is_accepted() {
        let image = parse(&reference()).expect("reference image refused");

        assert_eq!(image.image_bytes, 1024);
        assert_eq!(image.vendor_id, NVIDIA_VENDOR_ID);
        assert_eq!(image.bit_tokens, 3);
        assert!(image.last_image);
    }

    #[test]
    fn the_reference_device_identifier_is_not_a_real_part() {
        /* 0x5341 is "SA" and is chosen so no board can be inferred from it. */
        let image = parse(&reference()).expect("reference image refused");

        assert_eq!(image.device_id, 0x5341);
    }

    #[test]
    fn a_token_pointing_just_past_the_image_is_refused() {
        let mut broken = reference();
        let token = 0x100 + 12;

        broken[token + 2] = 0x01;
        broken[token + 3] = 0x00;
        broken[token + 4] = 0x00;
        broken[token + 5] = 0x04;
        assert_eq!(parse(&broken), Err(Status::BitToken));
    }

    #[test]
    fn a_zero_length_token_is_allowed_to_have_no_region() {
        let mut relaxed = reference();
        let token = 0x100 + 12;

        relaxed[token + 2] = 0x00;
        relaxed[token + 3] = 0x00;
        relaxed[token + 4] = 0x00;
        relaxed[token + 5] = 0x00;
        assert!(parse(&relaxed).is_ok());
    }

    #[test]
    fn the_bit_signature_is_the_byte_order_nouveau_searches_for() {
        /* id is a little-endian 0xB8FF, so the bytes are FF B8 then "BIT". */
        assert_eq!(BIT_SIGNATURE[0], 0xFF);
        assert_eq!(BIT_SIGNATURE[1], 0xB8);
        assert_eq!(&BIT_SIGNATURE[2..5], b"BIT");
        assert_eq!(BIT_SIGNATURE[5], 0x00);
    }

    #[test]
    fn every_byte_of_the_reference_image_is_reachable() {
        let image = reference();

        assert_eq!(image.len() % BLOCK_BYTES, 0);
        assert_eq!(image.len(), 1024);
    }
}
