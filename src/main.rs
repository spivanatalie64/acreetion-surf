// src/main.rs — Rust services for acreetion-surf
//
// The C shell (surf.c) owns the GTK/WebKit UI and calls these FFI functions
// for all file I/O, JSON, and subprocess work — where Rust's serde + zero-copy
// + memory safety shine. The C side talks to WebKit's native C API directly
// (zero binding overhead = fastest possible webview interaction).

use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::fs;
use std::io::Write;
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

// ─── Paths ────────────────────────────────────────────────────────────────────

fn home() -> String {
    std::env::var("HOME").unwrap_or_else(|_| "/home/natalie".to_string())
}

fn config_dir() -> PathBuf {
    PathBuf::from(&home()).join(".config").join("acreetion-surf")
}

// ─── Atomic write (tmp + fsync + rename) — crash-safe on the spinning disk ───

static WRITE_COUNTER: AtomicU64 = AtomicU64::new(0);

fn atomic_write(path: &Path, content: &str) -> std::io::Result<()> {
    let dir = path.parent().unwrap_or(Path::new("."));
    let pid = std::process::id();
    let ctr = WRITE_COUNTER.fetch_add(1, Ordering::Relaxed);
    let tmp = dir.join(format!(".tmp-{}-{}", pid, ctr));

    let mut f = fs::File::create(&tmp)?;
    f.write_all(content.as_bytes())?;
    f.flush()?;
    f.sync_all()?;
    drop(f);

    fs::rename(&tmp, path)?;
    Ok(())
}

// ─── String helpers for FFI ───────────────────────────────────────────────────

fn cstr_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned()
}

fn string_to_cstring_ptr(s: String) -> *mut c_char {
    CString::new(s).map_or(std::ptr::null_mut(), |c| c.into_raw())
}

/// Free a string returned by any surf_*_json function.
#[no_mangle]
pub extern "C" fn surf_free_str(ptr: *mut c_char) {
    if !ptr.is_null() {
        unsafe {
            drop(CString::from_raw(ptr));
        }
    }
}

// ─── History ──────────────────────────────────────────────────────────────────

#[derive(Serialize, Deserialize, Clone)]
struct HistoryEntry {
    url: String,
    title: String,
    time: i64,
}

fn history_path() -> PathBuf {
    config_dir().join("history.json")
}

fn now_unix() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Record a finished navigation. Dedupes consecutive identical URLs.
/// Called from C on WebKit LoadEvent::FINISHED.
#[no_mangle]
pub extern "C" fn surf_history_add(url: *const c_char, title: *const c_char) {
    let url = cstr_to_string(url);
    let title = cstr_to_string(title);
    if url.is_empty() {
        return;
    }

    let path = history_path();
    let mut history: Vec<HistoryEntry> = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Vec::new(),
    };

    let time = now_unix();
    if let Some(first) = history.first_mut() {
        if first.url == url {
            // Same as last — refresh title/time in place (no dupe)
            first.title = if title.is_empty() {
                first.title.clone()
            } else {
                title
            };
            first.time = time;
            let _ = atomic_write(&path, &serde_json::to_string_pretty(&history).unwrap());
            return;
        }
    }

    history.insert(0, HistoryEntry { url, title, time });
    history.truncate(500); // keep last 500

    let _ = atomic_write(&path, &serde_json::to_string_pretty(&history).unwrap());
}

/// Return the last `limit` history entries as JSON.
/// Caller must free with surf_free_str.
#[no_mangle]
pub extern "C" fn surf_history_json(limit: c_int) -> *mut c_char {
    let path = history_path();
    let history: Vec<HistoryEntry> = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Vec::new(),
    };

    let limit = if limit <= 0 { 20 } else { limit as usize };
    let sliced: Vec<&HistoryEntry> = history.iter().take(limit).collect();

    string_to_cstring_ptr(serde_json::to_string(&sliced).unwrap_or_default())
}

// ─── Bookmarks ────────────────────────────────────────────────────────────────

#[derive(Serialize, Deserialize, Clone)]
struct Bookmark {
    url: String,
    title: String,
}

fn bookmarks_path() -> PathBuf {
    config_dir().join("bookmarks.json")
}

/// Add a bookmark. Returns true if added, false if it already existed.
#[no_mangle]
pub extern "C" fn surf_bookmark_add(url: *const c_char, title: *const c_char) -> c_int {
    let url = cstr_to_string(url);
    let title = cstr_to_string(title);
    if url.is_empty() {
        return 0;
    }

    let path = bookmarks_path();
    let mut bookmarks: Vec<Bookmark> = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Vec::new(),
    };

    if bookmarks.iter().any(|b| b.url == url) {
        return 0; // already exists
    }

    bookmarks.push(Bookmark { url, title });
    let _ = atomic_write(&path, &serde_json::to_string_pretty(&bookmarks).unwrap());
    1
}

/// Return all bookmarks as JSON. Caller must free with surf_free_str.
#[no_mangle]
pub extern "C" fn surf_bookmarks_json() -> *mut c_char {
    let path = bookmarks_path();
    let bookmarks: Vec<Bookmark> = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Vec::new(),
    };
    string_to_cstring_ptr(serde_json::to_string(&bookmarks).unwrap_or_default())
}

// ─── Window geometry persistence (dual-monitor aware) ─────────────────────────

#[derive(Serialize, Deserialize)]
struct Geometry {
    w: i32,
    h: i32,
    x: i32,
    y: i32,
}

fn geometry_path() -> PathBuf {
    config_dir().join("geometry.json")
}

/// Load saved geometry. If no file, returns defaults (1600x900, centered).
#[no_mangle]
pub extern "C" fn surf_geometry_load(
    w: *mut c_int,
    h: *mut c_int,
    x: *mut c_int,
    y: *mut c_int,
) {
    let path = geometry_path();
    let geo: Geometry = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or(Geometry {
            w: 1600,
            h: 900,
            x: -1,
            y: -1,
        }),
        Err(_) => Geometry {
            w: 1600,
            h: 900,
            x: -1,
            y: -1,
        },
    };

    unsafe {
        if !w.is_null() {
            *w = geo.w;
        }
        if !h.is_null() {
            *h = geo.h;
        }
        if !x.is_null() {
            *x = geo.x;
        }
        if !y.is_null() {
            *y = geo.y;
        }
    }
}

/// Save current window geometry.
#[no_mangle]
pub extern "C" fn surf_geometry_save(w: c_int, h: c_int, x: c_int, y: c_int) {
    let geo = Geometry { w, h, x, y };
    let _ = atomic_write(
        &geometry_path(),
        &serde_json::to_string_pretty(&geo).unwrap(),
    );
}

// ─── Entry point — delegates to the C shell ───────────────────────────────────

use std::os::raw::c_char as cchar;

extern "C" {
    fn surf_main(argc: c_int, argv: *mut *mut cchar) -> c_int;
}

fn main() {
    // Machine-tuned env (only if not already set)
    // Intel UHD + X11: keep dmabuf renderer + compositing on.
    std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "0");
    // Disable web-process sandbox — real CPU win on the i5-10210U (personal
    // machine, not a shared host).
    if std::env::var("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS").is_err() {
        std::env::set_var("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
    }

    // Ensure config dir exists
    let _ = fs::create_dir_all(config_dir());

    // Pass argc/argv to the C surf_main
    let args: Vec<CString> = std::env::args()
        .map(|a| CString::new(a).unwrap_or_else(|_| CString::new("").unwrap()))
        .collect();
    let mut argv: Vec<*mut cchar> = args
        .iter()
        .map(|a| a.as_ptr() as *mut cchar)
        .collect();
    argv.push(std::ptr::null_mut()); // NULL terminator

    let ret = unsafe { surf_main(args.len() as c_int, argv.as_mut_ptr()) };
    std::process::exit(ret);
}