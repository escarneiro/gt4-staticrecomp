//! Moves files and folders to the Recycle Bin (SHFileOperationW with FOF_ALLOWUNDO), so a delete
//! in the launcher can always be undone from the bin.
//!
//! On Linux (TASK-074) the same promise holds through the freedesktop.org Trash specification 1.0, the trash of GNOME,
//! KDE and the file managers: an item on the home folder's file system goes to `$XDG_DATA_HOME/Trash` (default
//! `~/.local/share/Trash`); an item on another file system goes to `$topdir/.Trash-$uid` of that mount, so nothing is
//! copied across disks. Each item gets an `info/<name>.trashinfo` (created exclusively, then the item is renamed into
//! `files/<name>`), so the file manager can restore it.

use std::path::Path;

/// Moves one file or folder to the Recycle Bin without confirmation dialogs.
#[cfg(windows)]
pub fn move_to_recycle_bin(path: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::PCWSTR;
    use windows::Win32::Foundation::HWND;
    use windows::Win32::UI::Shell::{
        SHFileOperationW, FOF_ALLOWUNDO, FOF_NOCONFIRMATION, FOF_NOERRORUI, FOF_SILENT, FO_DELETE, SHFILEOPSTRUCTW,
    };

    // pFrom is a double-null-terminated list of paths.
    let mut wide: Vec<u16> = path.as_os_str().encode_wide().collect();
    wide.push(0);
    wide.push(0);
    let mut operation = SHFILEOPSTRUCTW {
        hwnd: HWND(std::ptr::null_mut()),
        wFunc: FO_DELETE,
        pFrom: PCWSTR(wide.as_ptr()),
        fFlags: (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI).0 as u16,
        ..Default::default()
    };
    // SAFETY: `operation` and the buffer behind `pFrom` live until the call returns.
    let result = unsafe { SHFileOperationW(&mut operation) };
    if result != 0 {
        return Err(format!("could not move {} to the Recycle Bin (error {result:#x})", path.display()));
    }
    if operation.fAnyOperationsAborted.as_bool() {
        return Err(format!("the delete of {} was cancelled", path.display()));
    }
    Ok(())
}

#[cfg(target_os = "linux")]
pub fn move_to_recycle_bin(path: &Path) -> Result<(), String> {
    let home_trash = trash::home_trash().ok_or_else(|| "neither XDG_DATA_HOME nor HOME is set: no trash folder".to_string())?;
    let stamp = trash::deletion_date(&crate::system::local_time());
    trash::move_to_trash(path, &home_trash, &stamp)
}

#[cfg(not(any(windows, target_os = "linux")))]
pub fn move_to_recycle_bin(path: &Path) -> Result<(), String> {
    Err(format!("the Recycle Bin is not available on this platform: {}", path.display()))
}

/// TASK-074: the freedesktop.org trash (Trash specification 1.0).
#[cfg(target_os = "linux")]
pub mod trash {
    use std::fs::{self, OpenOptions};
    use std::io::Write;
    use std::os::unix::fs::{DirBuilderExt, MetadataExt};
    use std::path::{Component, Path, PathBuf};

    unsafe extern "C" {
        fn getuid() -> u32;
    }

    /// `$XDG_DATA_HOME/Trash`, else `$HOME/.local/share/Trash`.
    pub fn home_trash() -> Option<PathBuf> {
        let absolute = |name: &str| std::env::var_os(name).map(PathBuf::from).filter(|path| path.is_absolute());
        absolute("XDG_DATA_HOME")
            .map(|data| data.join("Trash"))
            .or_else(|| absolute("HOME").map(|home| home.join(".local/share/Trash")))
    }

    /// The `DeletionDate` value: local time as `YYYY-MM-DDThh:mm:ss`.
    pub fn deletion_date(now: &gt4l_core::logs::LocalTime) -> String {
        format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", now.year, now.month, now.day, now.hour, now.minute, now.second)
    }

    /// The `Path` value: every byte outside the URI unreserved set and `/` percent-encoded (RFC 2396, as the
    /// specification asks), so names with spaces or accents survive.
    pub fn encode_path(path: &Path) -> String {
        use std::os::unix::ffi::OsStrExt;
        let mut out = String::new();
        for &byte in path.as_os_str().as_bytes() {
            if byte.is_ascii_alphanumeric() || b"-_.~/".contains(&byte) {
                out.push(byte as char);
            } else {
                out.push_str(&format!("%{byte:02X}"));
            }
        }
        out
    }

    /// The `.trashinfo` text of an item.
    pub fn info_text(path_value: &str, deletion_date: &str) -> String {
        format!("[Trash Info]\nPath={path_value}\nDeletionDate={deletion_date}\n")
    }

    /// The top folder of the mount that holds `path`: the last ancestor on the same device.
    fn mount_top(path: &Path, device: u64) -> PathBuf {
        let mut top = path.to_path_buf();
        while let Some(parent) = top.parent() {
            match fs::metadata(parent) {
                Ok(meta) if meta.dev() == device => top = parent.to_path_buf(),
                _ => break,
            }
        }
        top
    }

    fn private_dir(path: &Path) -> Result<(), String> {
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(path)
            .map_err(|error| format!("cannot create the trash folder {}: {error}", path.display()))
    }

    /// Moves `item` into the trash for its file system. `home_trash` is the home trash folder (used when `item` is on
    /// the same file system as it, or as its parent folder when that does not exist yet).
    pub fn move_to_trash(item: &Path, home_trash: &Path, deletion_date: &str) -> Result<(), String> {
        let meta = fs::symlink_metadata(item).map_err(|error| format!("cannot delete {}: {error}", item.display()))?;
        let item = std::path::absolute(item).map_err(|error| format!("cannot delete {}: {error}", item.display()))?;
        if item.components().any(|part| matches!(part, Component::ParentDir)) || item.parent().is_none() {
            return Err(format!("refusing to move {} to the trash", item.display()));
        }
        // The home trash, when it lives (or will live) on the item's file system.
        let home_probe = home_trash.ancestors().find(|folder| folder.exists()).map(Path::to_path_buf);
        let home_device = home_probe.as_deref().and_then(|folder| fs::metadata(folder).ok()).map(|meta| meta.dev());
        let (trash, path_value) = if home_device == Some(meta.dev()) {
            (home_trash.to_path_buf(), encode_path(&item))
        } else {
            // The specification's $topdir/.Trash-$uid, with Path relative to the top folder.
            let top = mount_top(&item, meta.dev());
            // SAFETY: getuid has no preconditions and cannot fail.
            let uid = unsafe { getuid() };
            let relative = item.strip_prefix(&top).map_err(|_| format!("{} is not below its mount {}", item.display(), top.display()))?;
            (top.join(format!(".Trash-{uid}")), encode_path(relative))
        };
        let files = trash.join("files");
        let info = trash.join("info");
        private_dir(&files)?;
        private_dir(&info)?;
        let base = item.file_name().and_then(|name| name.to_str()).unwrap_or("item").to_string();
        for attempt in 1..=1000u32 {
            let name = if attempt == 1 { base.clone() } else { format!("{base}.{attempt}") };
            let info_path = info.join(format!("{name}.trashinfo"));
            // The info file is created exclusively first: it reserves the name (specification, "Contents of a trash").
            let mut file = match OpenOptions::new().write(true).create_new(true).open(&info_path) {
                Ok(file) => file,
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(format!("cannot write {}: {error}", info_path.display())),
            };
            let target = files.join(&name);
            if target.exists() {
                let _ = fs::remove_file(&info_path);
                continue;
            }
            if let Err(error) = file.write_all(info_text(&path_value, deletion_date).as_bytes()).and_then(|_| file.sync_all()) {
                let _ = fs::remove_file(&info_path);
                return Err(format!("cannot write {}: {error}", info_path.display()));
            }
            return match fs::rename(&item, &target) {
                Ok(()) => Ok(()),
                Err(error) => {
                    let _ = fs::remove_file(&info_path);
                    Err(format!("could not move {} to the trash: {error}", item.display()))
                }
            };
        }
        Err(format!("the trash already holds 1000 items named {base}"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_path_is_refused_and_nothing_is_deleted() {
        let missing = std::env::temp_dir().join(format!("gt4l-recycle-missing-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&missing);
        assert!(move_to_recycle_bin(&missing).is_err());
    }

    #[cfg(target_os = "linux")]
    fn scratch(name: &str) -> std::path::PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-trash-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn a_folder_goes_to_the_home_trash_with_its_info_and_a_second_one_gets_its_own_name() {
        let root = scratch("home");
        let trash_dir = root.join("Trash");
        for round in 1..=2 {
            let slot = root.join("Save 02");
            std::fs::create_dir_all(slot.join("BASCUS-97328GAMEDATA")).unwrap();
            std::fs::write(slot.join("BASCUS-97328GAMEDATA").join("garage"), format!("round {round}")).unwrap();
            trash::move_to_trash(&slot, &trash_dir, "2026-11-26T08:40:00").unwrap();
            assert!(!slot.exists(), "moved, not copied");
        }
        let first = trash_dir.join("files/Save 02/BASCUS-97328GAMEDATA/garage");
        let second = trash_dir.join("files/Save 02.2/BASCUS-97328GAMEDATA/garage");
        assert_eq!(std::fs::read_to_string(first).unwrap(), "round 1");
        assert_eq!(std::fs::read_to_string(second).unwrap(), "round 2");
        let info = std::fs::read_to_string(trash_dir.join("info/Save 02.trashinfo")).unwrap();
        let expected_path = trash::encode_path(&root.join("Save 02"));
        assert_eq!(info, format!("[Trash Info]\nPath={expected_path}\nDeletionDate=2026-11-26T08:40:00\n"));
        assert!(expected_path.ends_with("/Save%2002"), "{expected_path}");
        assert!(trash_dir.join("info/Save 02.2.trashinfo").is_file());
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(std::fs::metadata(trash_dir.join("files")).unwrap().permissions().mode() & 0o777, 0o700);
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn the_info_names_are_percent_encoded_and_dates_are_local_iso() {
        assert_eq!(trash::encode_path(std::path::Path::new("/home/a/Save 01/ação")), "/home/a/Save%2001/a%C3%A7%C3%A3o");
        assert_eq!(trash::encode_path(std::path::Path::new("rel/x-y_z.~")), "rel/x-y_z.~");
        let now = gt4l_core::logs::LocalTime { year: 2026, month: 1, day: 2, hour: 3, minute: 4, second: 5 };
        assert_eq!(trash::deletion_date(&now), "2026-01-02T03:04:05");
    }
}
