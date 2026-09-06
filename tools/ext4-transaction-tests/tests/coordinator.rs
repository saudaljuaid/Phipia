// SPDX-License-Identifier: GPL-3.0-only
//! Exercise the production coordinator with the existing e2fsprogs fixture.

#[allow(dead_code)]
#[path = "../../../src/rust/ext4.rs"]
mod ext4;

use ext4::Status;
use std::cell::RefCell;
use std::path::PathBuf;

#[derive(Clone, Debug, Eq, PartialEq)]
enum Event {
    Write(u64, Vec<u8>),
    Flush(u32),
}

#[derive(Default)]
struct Device {
    bytes: Vec<u8>,
    events: Vec<Event>,
    fail_event: Option<usize>,
    accept_failed_write: bool,
    fail_superblock_read: bool,
    failed_reads: usize,
}

thread_local! {
    static DEVICE: RefCell<Device> = RefCell::new(Device::default());
}

// These are the production adapter's three native I/O callbacks. No upstream
// Ext4 writer receives the device; only the coordinator's executor calls write.
mod abi {
    use super::{DEVICE, Event};

    pub fn ext4_block_read(context: usize, start: u64, output: &mut [u8]) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            if device.fail_superblock_read && start == 1024 && output.len() == 1024 {
                device.failed_reads += 1;
                return false;
            }
            let Ok(start) = usize::try_from(start) else {
                return false;
            };
            let Some(end) = start.checked_add(output.len()) else {
                return false;
            };
            let Some(bytes) = device.bytes.get(start..end) else {
                return false;
            };
            output.copy_from_slice(bytes);
            true
        })
    }

    pub fn ext4_block_write(context: usize, start: u64, input: &[u8]) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            let failed = device.fail_event == Some(device.events.len());
            device.events.push(Event::Write(start, input.to_vec()));
            if !failed || device.accept_failed_write {
                let Ok(start) = usize::try_from(start) else {
                    return false;
                };
                let Some(end) = start.checked_add(input.len()) else {
                    return false;
                };
                let Some(bytes) = device.bytes.get_mut(start..end) else {
                    return false;
                };
                bytes.copy_from_slice(input);
            }
            !failed
        })
    }

    pub fn ext4_block_flush(context: usize, boundary: u32) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            let failed = device.fail_event == Some(device.events.len());
            device.events.push(Event::Flush(boundary));
            !failed
        })
    }
}

fn fixture() -> Option<PathBuf> {
    let Some(path) = std::env::var_os("PHIPIA_EXT4_RUST_FIXTURE") else {
        eprintln!("coordinator fixture unavailable; no Linux interoperability gate claimed");
        return None;
    };
    let path = PathBuf::from(path);
    assert!(path.is_file(), "configured coordinator fixture must exist");
    Some(path)
}

fn mount_fixture(path: &std::path::Path) -> Box<ext4::Mounted> {
    let bytes = std::fs::read(path).unwrap();
    mount_bytes(bytes)
}

fn mount_bytes(bytes: Vec<u8>) -> Box<ext4::Mounted> {
    let length = bytes.len() as u64;
    DEVICE.with_borrow_mut(|device| {
        *device = Device {
            bytes,
            ..Device::default()
        }
    });
    ext4::mount(1, length).unwrap().0
}

fn assert_public_reads_refused(mounted: &ext4::Mounted) {
    assert_eq!(ext4::free_bytes(mounted), Err(Status::Io));
    assert_eq!(ext4::stat(mounted, b"system/README.TXT"), Err(Status::Io));
    let mut bytes = [0xa5; 16];
    assert_eq!(
        ext4::pread(mounted, b"system/README.TXT", 0, &mut bytes),
        Err(Status::Io)
    );
    assert_eq!(bytes, [0xa5; 16]);
    assert!(matches!(
        ext4::directory_entry(mounted, b"system", 0),
        Err(Status::Io)
    ));
    assert!(ext4::unmount(mounted).is_err());
}

fn fsck(path: &std::path::Path, suffix: &str) {
    let output = path.with_extension(format!("{suffix}.img"));
    DEVICE.with_borrow(|device| std::fs::write(&output, &device.bytes).unwrap());
    let result = std::process::Command::new("e2fsck")
        .args(["-f", "-n"])
        .arg(&output)
        .output()
        .unwrap();
    let log = format!(
        "{}\n{}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
    std::fs::write(output.with_extension("e2fsck.txt"), &log).unwrap();
    println!("ext4 coordinator fsck {suffix}:\n{log}");
    let hash = std::process::Command::new("sha256sum").arg(&output).output().unwrap();
    assert!(hash.status.success(), "could not hash coordinator disk");
    println!("ext4 coordinator disk {}", String::from_utf8_lossy(&hash.stdout));
    assert!(result.status.success(), "e2fsck rejected {suffix}: {log}");
}

fn read_exact(mounted: &ext4::Mounted, path: &[u8], output: &mut [u8]) {
    let mut read = 0;
    while read < output.len() {
        let count = ext4::pread(mounted, path, read as u64, &mut output[read..]).unwrap();
        assert!(count > 0, "unexpected EOF at {read}");
        read += count;
    }
}

#[test]
fn rollback_reload_failure_drops_allocator_view_and_sync_retries() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    // Allocate 65 blocks using split writes, then exceed the still-bounded
    // single-transaction revoke capacity to force truncate rollback.
    let size = ext4::stat(&mounted, b"system/README.TXT").unwrap().size;
    ext4::transaction_probe(&mut mounted, b"system/README.TXT",
        size.div_ceil(4096) * 4096, &vec![0x52; 64 * 4096]).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    let original = ext4::stat(&mounted, b"system/README.TXT").unwrap();
    let before = DEVICE.with_borrow(|device| device.bytes.clone());
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = true;
    });
    assert_eq!(
        ext4::truncate_probe(&mut mounted, b"system/README.TXT", 0),
        Err(Status::Io)
    );
    DEVICE.with_borrow(|device| {
        assert!(device.failed_reads > 0, "rollback reload was not injected");
        assert!(
            device.events.is_empty(),
            "upstream mutation wrote to the device"
        );
        assert_eq!(device.bytes, before);
    });
    assert_public_reads_refused(&mounted);
    assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, b"system/README.TXT"), Ok(original));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-rollback");
}

#[test]
fn commit_reload_failure_hides_view_and_retry_does_not_rewrite_storage() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/reload-test", 0o600).unwrap();
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = true;
    });
    assert_eq!(
        ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"),
        Err(Status::Io)
    );
    DEVICE.with_borrow(|device| {
        assert_eq!(device.events.last(), Some(&Event::Flush(5)));
        assert!(device.failed_reads > 0);
    });
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = false;
    });
    assert_eq!(
        ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"),
        Ok(5)
    );
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    let mut bytes = [0; 5];
    assert_eq!(
        ext4::pread(&mounted, b"system/reload-test", 0, &mut bytes),
        Ok(5)
    );
    assert_eq!(&bytes, b"saved");
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = true);
    assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
    DEVICE.with_borrow(|device| assert_eq!(device.events.last(), Some(&Event::Flush(0))));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = false;
    });
    ext4::sync(&mut mounted).unwrap();
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-reload");
}

#[test]
fn pending_commit_is_hidden_and_every_storage_refusal_retries_exact_bytes() {
    let Some(path) = fixture() else { return };
    for case in 0..9 {
        let mut mounted = mount_fixture(&path);
        if case != 0 && case < 5 {
            if case == 4 {
                ext4::create_directory_probe(&mut mounted, b"system/retry-test").unwrap();
            } else {
                ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            }
            if case == 2 {
                ext4::transaction_probe(&mut mounted, b"system/retry-test", 0, &vec![0x5a; 8192])
                    .unwrap();
            }
            ext4::sync(&mut mounted).unwrap();
        }
        if case == 7 {
            ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            ext4::create_file_probe(&mut mounted, b"data/user/retry-test", 0o600).unwrap();
            ext4::transaction_probe(&mut mounted, b"data/user/retry-test", 0, &vec![0x33; 8192]).unwrap();
            ext4::sync(&mut mounted).unwrap();
        }
        if case == 8 {
            ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            ext4::transaction_probe(&mut mounted, b"system/retry-test", 0, &vec![0x71; 4093]).unwrap();
            ext4::sync(&mut mounted).unwrap();
        }
        let initial = DEVICE.with_borrow_mut(|device| {
            device.events.clear();
            device.bytes.clone()
        });
        drop(mounted);
        let mut mounted = mount_bytes(initial.clone());
        let target = if case == 5 { b"README.TXT".to_vec() } else { vec![b'x'; 97] };
        let mutate = |mounted: &mut ext4::Mounted| match case {
            0 => ext4::create_file_probe(mounted, b"system/retry-test", 0o600),
            1 => ext4::transaction_probe(mounted, b"system/retry-test", 4095, b"cross-block")
                .map(|_| ()),
            2 => ext4::truncate_probe(mounted, b"system/retry-test", 101),
            3 | 4 => ext4::rename_probe(mounted, b"system/retry-test", b"data/user/retry-test"),
            7 => ext4::rename_replace_probe(mounted, b"system/retry-test", b"data/user/retry-test"),
            8 => ext4::append_probe(mounted, b"system/retry-test", b"append-retry", 16384)
                .map(|result| assert_eq!(result, (4093, 12))),
            _ => ext4::symlink_probe(mounted, b"system/retry-test", &target),
        };
        mutate(&mut mounted).unwrap();
        let expected = DEVICE.with_borrow(|device| device.events.clone());
        if case == 1 {
            assert!(
                expected.contains(&Event::Flush(1)),
                "ordered data not exercised"
            );
        }
        ext4::sync(&mut mounted).unwrap();
        let expected_disk = DEVICE.with_borrow(|device| device.bytes.clone());
        drop(mounted);
        // Both a write rejected before acceptance and a write accepted with a lost
        // completion must retain the same plan. Flush errors never imply durability.
        for accept in [false, true] {
            for failed_at in 0..expected.len() {
                let mut mounted = mount_bytes(initial.clone());
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(failed_at);
                    device.accept_failed_write = accept;
                });
                assert_eq!(
                    mutate(&mut mounted),
                    Err(Status::Io),
                    "event {failed_at}, accepted {accept}"
                );
                let failed_prefix = DEVICE.with_borrow(|device| device.events.clone());
                assert_eq!(failed_prefix, expected[..=failed_at]);
                if failed_at >= 2 {
                    assert_public_reads_refused(&mounted);
                    assert_eq!(
                        ext4::create_file_probe(&mut mounted, b"system/other", 0o600),
                        Err(Status::Invalid)
                    );
                }
                DEVICE.with_borrow_mut(|device| {
                    device.events.clear();
                    device.fail_event = None;
                });
                mutate(&mut mounted).unwrap();
                let retry = DEVICE.with_borrow(|device| device.events.clone());
                let phase_start = if failed_at < 2 {
                    0
                } else if failed_at >= expected.len() - 2 {
                    expected.len() - 2
                } else {
                    2
                };
                assert_eq!(retry, expected[phase_start..], "retry event {failed_at}");
                let name: &[u8] = if case == 3 || case == 4 || case == 7 { b"data/user/retry-test" } else { b"system/retry-test" };
                if case == 5 || case == 6 {
                    let mut bytes = vec![0; target.len()];
                    assert_eq!(ext4::readlink(&mounted, name, &mut bytes), Ok(target.len()));
                    assert_eq!(bytes, target);
                } else {
                    assert_eq!(ext4::stat(&mounted, name).unwrap().links, if case == 4 { 2 } else { 1 });
                }
                ext4::sync(&mut mounted).unwrap();
                ext4::unmount(&mounted).unwrap();
                DEVICE.with_borrow(|device| {
                    assert_eq!(
                        device.bytes, expected_disk,
                        "case {case}, event {failed_at}, accepted {accept}"
                    )
                });
            }
        }
        // Every refusal converged byte-for-byte to this same image, including
        // counters, journal sequence and bitmap/metadata checksums.
        fsck(&path, &format!("coordinator-retry-case-{case}"));
    }
}

#[test]
fn append_uses_live_eof_through_aliases_and_refuses_overflow_without_writes() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/append-file";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::symlink_probe(&mut mounted, b"system/append-alias", b"append-file").unwrap();
    let first = vec![0x63; 4093];
    assert_eq!(ext4::append_probe(&mut mounted, name, &first, 16384), Ok((0, 4093)));
    assert_eq!(ext4::append_probe(&mut mounted, b"system/append-alias", b"second", 16384), Ok((4093, 6)));
    assert_eq!(ext4::append_probe(&mut mounted, name, b"third", 16384), Ok((4099, 5)));
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::append_probe(&mut mounted, name, b"overflow", 4104), Err(Status::Range));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    ext4::sync(&mut mounted).unwrap();
    let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(bytes);
    let mut actual = vec![0; 4104];
    read_exact(&mounted, name, &mut actual);
    let mut expected = first;
    expected.extend_from_slice(b"secondthird");
    assert_eq!(actual, expected);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-append");
}

#[test]
fn partial_truncate_zeroes_retained_tail_and_keeps_holes_sparse() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/truncate-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for size in [1, 4095, 4096, 4097] {
        assert_eq!(
            ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]),
            Ok(8192)
        );
        ext4::truncate_probe(&mut mounted, name, size).unwrap();
        let free_after_shrink = ext4::free_bytes(&mounted).unwrap();
        ext4::truncate_probe(&mut mounted, name, 8192).unwrap();
        assert_eq!(ext4::free_bytes(&mounted), Ok(free_after_shrink));
        let mut result = vec![0xa5; 8192];
        read_exact(&mounted, name, &mut result);
        assert!(result[..size as usize].iter().all(|byte| *byte == 0x5a));
        assert!(result[size as usize..].iter().all(|byte| *byte == 0));
    }
    // Shortening a sparse file in a hole must neither allocate nor initialize it.
    ext4::truncate_probe(&mut mounted, name, 0).unwrap();
    let free_empty = ext4::free_bytes(&mounted).unwrap();
    ext4::truncate_probe(&mut mounted, name, 16384).unwrap();
    ext4::truncate_probe(&mut mounted, name, 7001).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free_empty));
    ext4::transaction_probe(&mut mounted, name, 8000, b"end").unwrap();
    let mut result = vec![0xa5; 8003];
    read_exact(&mounted, name, &mut result);
    assert!(result[..8000].iter().all(|byte| *byte == 0));
    assert_eq!(&result[8000..], b"end");
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-truncate");
}

#[test]
fn partial_truncate_replays_size_and_zeroed_tail_together_at_every_barrier() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/truncate-cut";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.clone());
    assert!(
        events.contains(&Event::Flush(3)),
        "truncate never committed"
    );
    drop(mounted);
    let mut prefix = initial;
    let mut committed = false;
    for (index, event) in events.iter().enumerate() {
        match event {
            Event::Write(start, bytes) => {
                let start = *start as usize;
                prefix[start..start + bytes.len()].copy_from_slice(bytes);
            }
            Event::Flush(boundary) => {
                committed |= *boundary == 3;
                // A durable prefix ending at this acknowledged flush is the
                // backing of a new mount; no original stage or ring survives.
                DEVICE.with_borrow_mut(|device| {
                    *device = Device {
                        bytes: prefix.clone(),
                        ..Device::default()
                    }
                });
                let mut recovered = ext4::mount(1, prefix.len() as u64).unwrap().0;
                let expected_size = if committed { 101 } else { 8192 };
                assert_eq!(ext4::stat(&recovered, name).unwrap().size, expected_size);
                ext4::truncate_probe(&mut recovered, name, 8192).unwrap();
                let mut result = vec![0xa5; 8192];
                read_exact(&recovered, name, &mut result);
                assert!(result[..101].iter().all(|byte| *byte == 0x5a));
                let expected_tail = if committed { 0 } else { 0x5a };
                assert!(result[101..].iter().all(|byte| *byte == expected_tail));
                ext4::sync(&mut recovered).unwrap();
                ext4::unmount(&recovered).unwrap();
                fsck(&path, &format!("coordinator-truncate-cut-{index}"));
            }
        }
    }
}

fn debugfs(image: &std::path::Path, command: &str) {
    let result = std::process::Command::new("debugfs")
        .args(["-w", "-R", command]).arg(image).output().unwrap();
    assert!(result.status.success(), "debugfs: {}", String::from_utf8_lossy(&result.stderr));
}

#[test]
fn real_enospc_rolls_back_allocations_and_immutable_truncate_is_readonly() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/full-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/immutable-test", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/immutable-test", 0, b"protected").unwrap();
    ext4::sync(&mut mounted).unwrap();
    let free_blocks = ext4::free_bytes(&mounted).unwrap() / 4096;
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-full-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    let filler = path.with_extension("coordinator-filler.bin");
    // Leave a few blocks so the failing request allocates before ENOSPC. The
    // reserve also covers debugfs's own extent metadata, checked after mounting.
    std::fs::write(&filler, vec![0x5a; (free_blocks as usize - 48) * 4096]).unwrap();
    debugfs(&image, &format!("write \"{}\" /system/filler", filler.display()));
    debugfs(&image, "set_inode_field /system/immutable-test flags 0x80010");
    let mut mounted = mount_fixture(&image);
    let available = ext4::free_bytes(&mounted).unwrap();
    assert!(available > 32 * 4096 && available < 64 * 4096);
    // One chunk fits, the second must roll back and return a durable short write.
    let written = ext4::transaction_probe(&mut mounted, name, 0, &vec![0x52; 64 * 4096]).unwrap();
    assert_eq!(written, 32 * 4096);
    assert_eq!(ext4::stat(&mounted, name).unwrap().size, written as u64);
    let mut tail = [0xa5; 1];
    assert_eq!(ext4::pread(&mounted, name, written as u64, &mut tail), Ok(0));
    let free = ext4::free_bytes(&mounted).unwrap();
    assert!(free > 0 && free < 32 * 4096, "fixture did not reach low space: {free}");
    let original = ext4::stat(&mounted, name).unwrap();
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, &vec![0x52; 32 * 4096]),
        Err(Status::Full));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, name), Ok(original));
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = true);
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, &vec![0x52; 32 * 4096]),
        Err(Status::Io));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, name), Ok(original));
    assert_eq!(ext4::truncate_probe(&mut mounted, b"system/immutable-test", 2),
        Err(Status::ReadOnly));
    let mut content = [0; 9];
    assert_eq!(ext4::pread(&mounted, b"system/immutable-test", 0, &mut content), Ok(9));
    assert_eq!(&content, b"protected");
    // A fresh, fitting allocation must still work after rolling back ENOSPC.
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, b"fits"), Ok(4));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-full");
}

#[test]
fn sync_finishes_failed_marker_activation_without_starting_the_mutation() {
    let Some(path) = fixture() else { return };
    for fail_at in [0, 1] {
        for accept in [false, true] {
            let mut mounted = mount_fixture(&path);
            let initial = DEVICE.with_borrow(|device| device.bytes.clone());
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(fail_at);
                device.accept_failed_write = accept;
            });
            assert_eq!(ext4::create_file_probe(&mut mounted, b"system/never-created", 0o600),
                Err(Status::Io));
            let first_attempt = DEVICE.with_borrow_mut(|device| {
                let events = device.events.clone();
                device.events.clear();
                events
            });
            // A second refusal through sync must keep the activation retryable.
            assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
            DEVICE.with_borrow_mut(|device| {
                assert_eq!(device.events, first_attempt);
                device.events.clear();
                device.fail_event = None;
            });
            ext4::sync(&mut mounted).unwrap();
            DEVICE.with_borrow(|device| {
                assert_eq!(&device.events[..first_attempt.len()], &first_attempt);
                assert_eq!(device.events.len(), 4, "only activation and clearing are needed");
                assert_eq!(device.bytes, initial, "sync published the failed create");
            });
            assert_eq!(ext4::stat(&mounted, b"system/never-created"), Err(Status::NotFound));
            ext4::unmount(&mounted).unwrap();
        }
    }
    fsck(&path, "coordinator-marker-sync");
}

#[test]
fn cross_directory_file_rename_preserves_identity_and_refuses_replacement() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, b"system/move-from").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/move-to").unwrap();
    let source = b"system/move-from/source";
    let target = b"system/move-to/target";
    ext4::create_file_probe(&mut mounted, source, 0o640).unwrap();
    ext4::transaction_probe(&mut mounted, source, 4095, b"across").unwrap();
    ext4::link_file_probe(&mut mounted, source, b"system/move-from/alias").unwrap();
    let identity = ext4::stat(&mounted, source).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::rename_probe(&mut mounted, source, target).unwrap();
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, target), Ok(identity));
    assert_eq!(ext4::stat(&mounted, b"system/move-from/alias"), Ok(identity));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    let mut content = vec![0xa5; 4101];
    read_exact(&mounted, target, &mut content);
    assert!(content[..4095].iter().all(|byte| *byte == 0));
    assert_eq!(&content[4095..], b"across");
    ext4::create_file_probe(&mut mounted, source, 0o600).unwrap();
    let other = ext4::stat(&mounted, source).unwrap();
    assert_eq!(ext4::rename_probe(&mut mounted, source, target), Err(Status::Exists));
    assert_eq!(ext4::stat(&mounted, source), Ok(other));
    assert_eq!(ext4::stat(&mounted, target), Ok(identity));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-move");
    drop(mounted);
    let image = path.with_extension("coordinator-move.img");
    debugfs(&image, "symlink /system/move-alias /system/move-from");
    let mut mounted = mount_fixture(&image);
    ext4::rename_probe(&mut mounted, source, b"system/move-alias/renamed").unwrap();
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, b"system/move-from/renamed"), Ok(other));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-move-alias");
}

#[test]
fn replacement_rename_preserves_hardlinks_and_directory_parent_counts() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    for parent in ["system", "data/user"] {
        let source = b"system/replace-source";
        let target = format!("{parent}/replace-target");
        ext4::create_file_probe(&mut mounted, source, 0o640).unwrap();
        ext4::transaction_probe(&mut mounted, source, 0, b"new value").unwrap();
        ext4::link_file_probe(&mut mounted, source, b"system/source-alias").unwrap();
        let source_inode = ext4::lstat(&mounted, source).unwrap();
        ext4::rename_replace_probe(&mut mounted, source, b"system/source-alias").unwrap();
        assert_eq!(ext4::lstat(&mounted, source), Ok(source_inode));
        assert_eq!(ext4::lstat(&mounted, b"system/source-alias"), Ok(source_inode));
        ext4::unlink_file_probe(&mut mounted, b"system/source-alias").unwrap();
        ext4::create_file_probe(&mut mounted, target.as_bytes(), 0o600).unwrap();
        ext4::transaction_probe(&mut mounted, target.as_bytes(), 0, b"old value").unwrap();
        ext4::link_file_probe(&mut mounted, target.as_bytes(), b"system/old-alias").unwrap();
        let old_inode = ext4::lstat(&mounted, target.as_bytes()).unwrap().inode;
        ext4::rename_replace_probe(&mut mounted, source, target.as_bytes()).unwrap();
        assert_eq!(ext4::lstat(&mounted, source), Err(Status::NotFound));
        assert_eq!(ext4::lstat(&mounted, target.as_bytes()).unwrap().inode, source_inode.inode);
        assert_eq!(ext4::lstat(&mounted, b"system/old-alias").unwrap().inode, old_inode);
        let mut data = [0; 9];
        read_exact(&mounted, target.as_bytes(), &mut data);
        assert_eq!(&data, b"new value");
        read_exact(&mounted, b"system/old-alias", &mut data);
        assert_eq!(&data, b"old value");
        ext4::unlink_file_probe(&mut mounted, target.as_bytes()).unwrap();
        ext4::unlink_file_probe(&mut mounted, b"system/old-alias").unwrap();
    }
    ext4::create_directory_probe(&mut mounted, b"system/source-dir").unwrap();
    ext4::create_directory_probe(&mut mounted, b"data/user/target-dir").unwrap();
    ext4::create_file_probe(&mut mounted, b"data/user/target-dir/child", 0o600).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"system/source-dir",
        b"data/user/target-dir"), Err(Status::NotEmpty));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"system/source-dir",
        b"data/user/target-dir/child"), Err(Status::NotDirectory));
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"data/user/target-dir/child",
        b"system/source-dir"), Err(Status::IsDirectory));
    ext4::unlink_file_probe(&mut mounted, b"data/user/target-dir/child").unwrap();
    let system_links = ext4::stat(&mounted, b"system").unwrap().links;
    let user_links = ext4::stat(&mounted, b"data/user").unwrap().links;
    ext4::rename_replace_probe(&mut mounted, b"system/source-dir", b"data/user/target-dir").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, system_links - 1);
    assert_eq!(ext4::stat(&mounted, b"data/user").unwrap().links, user_links);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-rename-replace");
}

#[test]
fn symlinks_roundtrip_inline_boundary_dangling_and_looping_targets() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let free = ext4::free_bytes(&mounted).unwrap();
    for length in [1, 59, 60, 61, 127, 4095] {
        let name = format!("system/link-{length}");
        // Avoid oversized individual components while preserving literal bytes.
        let mut target = vec![b'x'; length];
        for index in (1..length).step_by(2) { target[index] = b'/'; }
        ext4::symlink_probe(&mut mounted, name.as_bytes(), &target).unwrap();
        let metadata = ext4::lstat(&mounted, name.as_bytes()).unwrap();
        assert_eq!(metadata.file_type, 3);
        assert_eq!(metadata.mode & 0o777, 0o777);
        assert_eq!(metadata.links, 1);
        let mut bytes = vec![0xa5; length + 1];
        assert_eq!(ext4::readlink(&mounted, name.as_bytes(), &mut bytes), Ok(length));
        assert_eq!(&bytes[..length], &target);
        assert_eq!(bytes[length], 0xa5);
        assert_eq!(ext4::readlink(&mounted, name.as_bytes(), &mut bytes[..1]), Ok(1));
        assert_eq!(ext4::symlink_probe(&mut mounted, name.as_bytes(), b"other"), Err(Status::Exists));
    }
    ext4::symlink_probe(&mut mounted, b"system/link-loop", b"link-loop").unwrap();
    ext4::symlink_probe(&mut mounted, b"system/link-real", b"README.TXT").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system/link-real"), ext4::stat(&mounted, b"system/README.TXT"));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-symlinks-live");
    let disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(disk);
    let mut target = [0; 16];
    assert_eq!(ext4::readlink(&mounted, b"system/link-loop", &mut target), Ok(9));
    for length in [1, 59, 60, 61, 127, 4095] {
        let name = format!("system/link-{length}");
        ext4::rename_probe(&mut mounted, name.as_bytes(), b"data/user/moved-link").unwrap();
        ext4::unlink_file_probe(&mut mounted, b"data/user/moved-link").unwrap();
    }
    ext4::unlink_file_probe(&mut mounted, b"system/link-loop").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/link-real").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-symlinks-removed");
}

#[test]
fn sparse_fragmented_extent_tree_grows_overwrites_and_shrinks() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/fragmented";
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    // Separate logical blocks cannot merge even if physical allocation happens
    // contiguously. Sixty extents force allocation beyond the inode root.
    let mut expected = vec![0; 59 * 8192 + 4096];
    for index in 0..60usize {
        let start = index * 8192;
        let bytes = vec![(index + 1) as u8; 4096];
        assert_eq!(ext4::transaction_probe(&mut mounted, name, start as u64, &bytes), Ok(bytes.len()));
        expected[start..start + bytes.len()].copy_from_slice(&bytes);
    }
    let mut actual = vec![0xa5; expected.len()];
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    // Coalesce an unaligned overwrite across existing extents and holes.
    let replacement = vec![0xbc; 32 * 4096 + 11];
    ext4::transaction_probe(&mut mounted, name, 4093, &replacement).unwrap();
    expected[4093..4093 + replacement.len()].copy_from_slice(&replacement);
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-fragmented-before-truncate");
    let shortened = 40 * 8192 + 103;
    // Keep the upstream error visible instead of losing its precise cause in
    // the stable C Invalid status. This isolated stage never writes the device.
    let backing = DEVICE.with_borrow(|device| device.bytes.clone());
    let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(
        Box::new(backing.clone()), backing.len() as u64).unwrap());
    let raw = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()),
        Some(Box::new(stage.clone()))).unwrap();
    let mut raw_file = raw.open(b"/system/fragmented").unwrap();
    raw_file.truncate(shortened as u64).expect("upstream fragmented truncate");
    println!("fragmented truncate stage: {} images, {} revokes",
        stage.staged_block_count(), stage.revoked_block_count());
    drop(raw_file);
    drop(raw);
    drop(stage);
    ext4::truncate_probe(&mut mounted, name, shortened as u64).unwrap();
    ext4::truncate_probe(&mut mounted, name, expected.len() as u64).unwrap();
    expected[shortened..].fill(0);
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    // At most 58 live data blocks and one extent leaf remain after shrink.
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-fragmented-live");
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-fragmented-extents");
}

#[test]
fn directory_growth_shrink_and_multiblock_rmdir_preserve_counters() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/growing").unwrap();
    let mut names = Vec::new();
    for index in 0..55 {
        let name = format!("system/growing/{index:03}-{}", "x".repeat(80));
        ext4::create_directory_probe(&mut mounted, name.as_bytes()).unwrap();
        names.push(name);
    }
    assert!(ext4::stat(&mounted, b"system/growing").unwrap().size > 4096);
    // Reverse removal empties the tail block, requiring both the last child
    // block and the parent block to be revoked in the same rmdir transaction.
    for name in names.iter().rev() {
        ext4::remove_directory_probe(&mut mounted, name.as_bytes()).unwrap();
    }
    assert_eq!(ext4::stat(&mounted, b"system/growing").unwrap().size, 4096);
    ext4::remove_directory_probe(&mut mounted, b"system/growing").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::create_directory_probe(&mut mounted, b"system/empty-grown").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let image = path.with_extension("coordinator-grown-dir.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    drop(mounted);
    for _ in 0..3 { debugfs(&image, "expand_dir /system/empty-grown"); }
    let mut mounted = mount_fixture(&image);
    assert_eq!(ext4::stat(&mounted, b"system/empty-grown").unwrap().size, 4 * 4096);
    ext4::remove_directory_probe(&mut mounted, b"system/empty-grown").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-directory-shrink");
}

#[test]
fn directory_move_updates_parents_and_rejects_descendant_cycles() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let source = b"system/tree";
    let destination = b"data/user/tree";
    ext4::create_directory_probe(&mut mounted, source).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/tree/child").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/tree/child/file", 0o640).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/tree/child/file", 0, b"preserved").unwrap();
    let identity = ext4::stat(&mounted, source).unwrap();
    let old_parent = ext4::stat(&mounted, b"system").unwrap();
    let new_parent = ext4::stat(&mounted, b"data/user").unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    assert_eq!(ext4::rename_probe(&mut mounted, source,
        b"system/tree/child/cycle"), Err(Status::Invalid));
    assert_eq!(ext4::stat(&mounted, source), Ok(identity));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    // Remount so the successful trace includes marker activation from clean.
    drop(mounted);
    let mut mounted = mount_bytes(initial.clone());
    ext4::rename_probe(&mut mounted, source, destination).unwrap();
    let trace = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, destination), Ok(identity));
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, old_parent.links - 1);
    assert_eq!(ext4::stat(&mounted, b"data/user").unwrap().links, new_parent.links + 1);
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    drop(mounted);
    // Recover each acknowledged durable prefix. Before commit only the old
    // tree exists; after commit the new tree includes coherent parent links.
    let mut prefix = initial;
    let mut committed = false;
    for (index, event) in trace.iter().enumerate() {
        match event {
            Event::Write(start, bytes) => {
                let start = *start as usize;
                prefix[start..start + bytes.len()].copy_from_slice(bytes);
                continue;
            }
            Event::Flush(3) => committed = true,
            Event::Flush(_) => {}
        }
        let mounted = mount_bytes(prefix.clone());
        let (present, absent, content): (&[u8], &[u8], &[u8]) = if committed {
            (destination, source, b"data/user/tree/child/file")
        } else {
            (source, destination, b"system/tree/child/file")
        };
        assert_eq!(ext4::stat(&mounted, present), Ok(identity));
        assert_eq!(ext4::stat(&mounted, absent), Err(Status::NotFound));
        let mut bytes = [0; 9];
        read_exact(&mounted, content, &mut bytes);
        assert_eq!(&bytes, b"preserved");
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-directory-move-cut-{index}"));
    }
    let image = path.with_extension("coordinator-directory-alias.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    debugfs(&image, "symlink /system/tree-alias /data/user/tree/child");
    let mut mounted = mount_fixture(&image);
    assert_eq!(ext4::rename_probe(&mut mounted, destination,
        b"system/tree-alias/cycle"), Err(Status::Invalid));
    assert_eq!(ext4::rename_probe(&mut mounted, destination,
        b"data/user/tree/child/existing"), Err(Status::Invalid));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-directory-cycle");
}

#[test]
fn repeated_mount_recovery_refusals_converge_to_the_same_clean_disk() {
    let Some(path) = fixture() else { return };
    let name = b"system/recovery-refusal";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let mut crashed = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let committed = DEVICE.with_borrow(|device| device.events.clone());
    let mut saw_commit = false;
    for event in committed {
        match event {
            Event::Write(start, bytes) => {
                let start = start as usize;
                crashed[start..start + bytes.len()].copy_from_slice(&bytes);
            }
            Event::Flush(3) => { saw_commit = true; break; }
            Event::Flush(_) => {}
        }
    }
    assert!(saw_commit);
    drop(mounted);
    let recovered = mount_bytes(crashed.clone());
    assert_eq!(ext4::stat(&recovered, name).unwrap().size, 101);
    ext4::unmount(&recovered).unwrap();
    let (expected_events, expected_disk) = DEVICE.with_borrow(|device|
        (device.events.clone(), device.bytes.clone()));
    assert!(expected_events.contains(&Event::Flush(4)));
    assert!(expected_events.contains(&Event::Flush(5)));
    assert_eq!(expected_events.last(), Some(&Event::Flush(0)));
    drop(recovered);
    let mut repeated_failures = 0;
    for accept in [false, true] {
        for fail_at in 0..expected_events.len() {
            DEVICE.with_borrow_mut(|device| *device = Device {
                bytes: crashed.clone(), fail_event: Some(fail_at),
                accept_failed_write: accept, ..Device::default()
            });
            assert!(matches!(ext4::mount(1, crashed.len() as u64), Err(Status::Io)));
            DEVICE.with_borrow_mut(|device| {
                assert_eq!(device.events, expected_events[..=fail_at]);
                device.events.clear();
                device.fail_event = Some(0);
            });
            // Crash/refuse again using the first attempt's partly checkpointed
            // bytes, without retaining any failed mount object or replay plan.
            let second = ext4::mount(1, crashed.len() as u64);
            let recovered = match second {
                Ok((mounted, _)) => mounted, // the first clear may have reached disk
                Err(Status::Io) => {
                    repeated_failures += 1;
                    DEVICE.with_borrow_mut(|device| device.fail_event = None);
                    ext4::mount(1, crashed.len() as u64).unwrap().0
                }
                Err(error) => panic!("recovery became unrecoverable: {error:?}"),
            };
            assert_eq!(ext4::stat(&recovered, name).unwrap().size, 101);
            let mut prefix = [0; 101];
            read_exact(&recovered, name, &mut prefix);
            assert_eq!(prefix, [0x5a; 101]);
            ext4::unmount(&recovered).unwrap();
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, expected_disk,
                "recovery event {fail_at}, accepted {accept}"));
        }
    }
    assert!(repeated_failures > 0);
    fsck(&path, "coordinator-recovery-refusals");
}

#[test]
fn unaligned_write_spans_transactions_and_retries_only_the_unfinished_suffix() {
    let Some(path) = fixture() else { return };
    let name = b"system/split-write";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let source: Vec<u8> = (0..64 * 4096).map(|index| (index % 251) as u8).collect();
    let mut mounted = mount_bytes(initial.clone());
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Ok(source.len()));
    let expected = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(expected.iter().filter(|event| **event == Event::Flush(3)).count(), 3);
    let mut result = vec![0xa5; source.len() + 7];
    read_exact(&mounted, name, &mut result);
    assert_eq!(&result[..7], &[0; 7]);
    assert_eq!(&result[7..], &source);
    ext4::sync(&mut mounted).unwrap();
    let expected_disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let second_start = expected.iter().position(|event| *event == Event::Flush(5)).unwrap() + 1;
    let ordered_flush = second_start + expected[second_start..].iter()
        .position(|event| *event == Event::Flush(1)).unwrap();
    let commit_flush = second_start + expected[second_start..].iter()
        .position(|event| *event == Event::Flush(3)).unwrap();
    for fail_at in [second_start, ordered_flush, commit_flush] {
        for accept in [false, true] {
            for through_sync in [false, true] {
                let mut mounted = mount_bytes(initial.clone());
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(fail_at);
                    device.accept_failed_write = accept;
                });
                assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Err(Status::Io));
                assert_public_reads_refused(&mounted);
                assert_eq!(ext4::transaction_probe(&mut mounted, name, 8, &source), Err(Status::Invalid));
                DEVICE.with_borrow_mut(|device| {
                    assert_eq!(device.events, expected[..=fail_at]);
                    device.events.clear();
                    device.fail_event = None;
                });
                if through_sync {
                    ext4::sync(&mut mounted).unwrap();
                } else {
                    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Ok(source.len()));
                    ext4::sync(&mut mounted).unwrap();
                }
                DEVICE.with_borrow(|device| {
                    assert!(device.events.starts_with(&expected[second_start..]),
                        "earlier checkpointed chunks were repeated");
                    assert_eq!(device.events.len(), expected.len() - second_start + 2);
                    assert_eq!(device.bytes, expected_disk);
                });
                ext4::unmount(&mounted).unwrap();
            }
        }
    }
    fsck(&path, "coordinator-split-write");
}
