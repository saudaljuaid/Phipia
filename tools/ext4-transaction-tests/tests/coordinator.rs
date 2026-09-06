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
            let Ok(start) = usize::try_from(start) else { return false };
            let Some(end) = start.checked_add(output.len()) else { return false };
            let Some(bytes) = device.bytes.get(start..end) else { return false };
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
                let Ok(start) = usize::try_from(start) else { return false };
                let Some(end) = start.checked_add(input.len()) else { return false };
                let Some(bytes) = device.bytes.get_mut(start..end) else { return false };
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
    let length = bytes.len() as u64;
    DEVICE.with_borrow_mut(|device| *device = Device { bytes, ..Device::default() });
    ext4::mount(1, length).unwrap().0
}

fn assert_public_reads_refused(mounted: &ext4::Mounted) {
    assert_eq!(ext4::free_bytes(mounted), Err(Status::Io));
    assert_eq!(ext4::stat(mounted, b"system/README.TXT"), Err(Status::Io));
    let mut bytes = [0xa5; 16];
    assert_eq!(ext4::pread(mounted, b"system/README.TXT", 0, &mut bytes), Err(Status::Io));
    assert_eq!(bytes, [0xa5; 16]);
    assert!(matches!(ext4::directory_entry(mounted, b"system", 0), Err(Status::Io)));
    assert!(ext4::unmount(mounted).is_err());
}

fn fsck(path: &std::path::Path, suffix: &str) {
    let output = path.with_extension(format!("{suffix}.img"));
    DEVICE.with_borrow(|device| std::fs::write(&output, &device.bytes).unwrap());
    let result = std::process::Command::new("e2fsck")
        .args(["-f", "-n"]).arg(&output).output().unwrap();
    let log = format!("{}\n{}", String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr));
    std::fs::write(output.with_extension("e2fsck.txt"), &log).unwrap();
    assert!(result.status.success(), "e2fsck rejected {suffix}: {log}");
}

#[test]
fn rollback_reload_failure_drops_allocator_view_and_sync_retries() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    // Establish a durable recovery marker and a checkpointed, readable view.
    ext4::create_file_probe(&mut mounted, b"system/rollback-test", 0o640).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    let original = ext4::stat(&mounted, b"system/README.TXT").unwrap();
    let before = DEVICE.with_borrow(|device| device.bytes.clone());
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = true;
    });
    // 64 data blocks plus allocation metadata cannot fit the 64-image stage.
    // The write allocates in memory, then fails and attempts rollback/reload.
    let offset = original.size.div_ceil(4096) * 4096;
    assert_eq!(ext4::transaction_probe(&mut mounted, b"system/README.TXT",
        offset, &vec![0x52; 64 * 4096]), Err(Status::Io));
    DEVICE.with_borrow(|device| {
        assert!(device.failed_reads > 0, "rollback reload was not injected");
        assert!(device.events.is_empty(), "upstream mutation wrote to the device");
        assert_eq!(device.bytes, before);
    });
    assert_public_reads_refused(&mounted);
    assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, b"system/README.TXT"), Ok(original));
    ext4::unlink_file_probe(&mut mounted, b"system/rollback-test").unwrap();
    ext4::sync(&mut mounted).unwrap();
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
    assert_eq!(ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"),
        Err(Status::Io));
    DEVICE.with_borrow(|device| {
        assert_eq!(device.events.last(), Some(&Event::Flush(5)));
        assert!(device.failed_reads > 0);
    });
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = false;
    });
    assert_eq!(ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"), Ok(5));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    let mut bytes = [0; 5];
    assert_eq!(ext4::pread(&mounted, b"system/reload-test", 0, &mut bytes), Ok(5));
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
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
    let expected = DEVICE.with_borrow(|device| device.events.clone());
    drop(mounted);
    // Both a write rejected before acceptance and a write accepted with a lost
    // completion must retain the same plan. Flush errors never imply durability.
    for accept in [false, true] {
        for failed_at in 0..expected.len() {
            let mut mounted = mount_fixture(&path);
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(failed_at);
                device.accept_failed_write = accept;
            });
            assert_eq!(ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600),
                Err(Status::Io), "event {failed_at}, accepted {accept}");
            let failed_prefix = DEVICE.with_borrow(|device| device.events.clone());
            assert_eq!(failed_prefix, expected[..=failed_at]);
            if failed_at >= 2 {
                assert_public_reads_refused(&mounted);
                assert_eq!(ext4::create_file_probe(&mut mounted, b"system/other", 0o600),
                    Err(Status::Invalid));
            }
            DEVICE.with_borrow_mut(|device| {
                device.events.clear();
                device.fail_event = None;
            });
            ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            let retry = DEVICE.with_borrow(|device| device.events.clone());
            let phase_start = if failed_at < 2 { 0 }
                else if failed_at >= expected.len() - 2 { expected.len() - 2 }
                else { 2 };
            assert_eq!(retry, expected[phase_start..], "retry event {failed_at}");
            assert_eq!(ext4::stat(&mounted, b"system/retry-test").unwrap().links, 1);
            ext4::sync(&mut mounted).unwrap();
            ext4::unmount(&mounted).unwrap();
            fsck(&path, &format!("coordinator-retry-{accept}-{failed_at}"));
        }
    }
}

#[test]
fn partial_truncate_zeroes_retained_tail_and_keeps_holes_sparse() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/truncate-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for size in [1, 4095, 4096, 4097] {
        assert_eq!(ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]), Ok(8192));
        ext4::truncate_probe(&mut mounted, name, size).unwrap();
        let free_after_shrink = ext4::free_bytes(&mounted).unwrap();
        ext4::truncate_probe(&mut mounted, name, 8192).unwrap();
        assert_eq!(ext4::free_bytes(&mounted), Ok(free_after_shrink));
        let mut result = vec![0xa5; 8192];
        assert_eq!(ext4::pread(&mounted, name, 0, &mut result), Ok(8192));
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
    assert_eq!(ext4::pread(&mounted, name, 0, &mut result), Ok(8003));
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
    assert!(events.contains(&Event::Flush(3)), "truncate never committed");
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
                DEVICE.with_borrow_mut(|device| *device = Device {
                    bytes: prefix.clone(), ..Device::default()
                });
                let mut recovered = ext4::mount(1, prefix.len() as u64).unwrap().0;
                let expected_size = if committed { 101 } else { 8192 };
                assert_eq!(ext4::stat(&recovered, name).unwrap().size, expected_size);
                ext4::truncate_probe(&mut recovered, name, 8192).unwrap();
                let mut result = vec![0xa5; 8192];
                assert_eq!(ext4::pread(&recovered, name, 0, &mut result), Ok(8192));
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
