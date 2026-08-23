// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the bounded BusyBox FAT16 parser.

// This harness imports the complete inherited module while exercising only the
// new chain API; the kernel crate and inherited standalone test use the rest.
#![allow(dead_code)]

#[path = "../src/rust/fat16.rs"]
mod fat16;
#[path = "../src/rust/linux_fat16.rs"]
mod linux_fat16;

static BUSYBOX: &[u8] = include_bytes!(env!("SAPOTE_BUSYBOX_BINARY"));

#[test]
fn pinned_busybox_payload_is_exact() {
    let payload = linux_fat16::validate_payload(BUSYBOX).unwrap();
    assert_eq!(payload.byte_count, linux_fat16::FILE_BYTES);
    assert_eq!(payload.deterministic, 1);
    assert_eq!(payload.sha256, linux_fat16::BUSYBOX_SHA256);

    for length in [0, 1, BUSYBOX.len() - 1] {
        assert!(matches!(
            linux_fat16::validate_payload(&BUSYBOX[..length]),
            Err(linux_fat16::Status::PayloadLength)
        ));
    }
    let mut changed = BUSYBOX.to_vec();
    changed[BUSYBOX.len() / 2] ^= 1;
    assert!(matches!(
        linux_fat16::validate_payload(&changed),
        Err(linux_fat16::Status::PayloadDigest)
    ));
}
