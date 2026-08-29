// SPDX-License-Identifier: GPL-3.0-only
#![deny(warnings)]
#![allow(dead_code)]

#[path = "../src/rust/sha256.rs"]
mod sha256;
#[path = "../src/rust/native_image.rs"]
mod native_image;

fn main() {}

#[test]
fn linked_sdk_application_is_general_static_elf() {
    let Ok(path) = std::env::var("SAPOTE_NATIVE_TEST_ELF") else { return; };
    let bytes = std::fs::read(path).expect("read SDK application");
    let image = native_image::parse_elf(&bytes).expect("validate SDK application");
    assert_eq!(image.segment_count, 3);
    assert_ne!(image.tls.memory_size, 0);
}
