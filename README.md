# AcreetionOS Surf v2.0 — C + Rust native WebKitGTK browser

Rewritten from Python+GObject-introspection to a **native C + Rust** binary.
Zero binding overhead, zero Python runtime, instant startup.

## Architecture

| Layer | Language | Role |
|---|---|---|
| `src/surf.c` | **C** | GTK3 + WebKitGTK 4.1 browser shell — window, tabs, toolbar, signals, downloads, dark-mode stylesheet, IPC socket, keyboard shortcuts. Talks to WebKit's **C API directly** (no marshalling per call). |
| `src/main.rs` | **Rust** | Services library via FFI: history (serde + dedup + atomic writes), bookmarks, geometry persistence, entry point + env tuning. |

One ELF binary (~450 K, stripped, LTO'd). Rust is the linker; `build.rs`
compiles the C shell via the `cc` crate and links `gtk-3`, `webkit2gtk-4.1`,
`json-glib`.

## Why it's faster than the Python version

- **No Python interpreter** — no cold-start, no runtime RSS (~30-50 MB saved)
- **No GI marshalling** — every WebKit call was going through Python→GObject
  introspection; now it's a direct C call
- **Rust serde** for JSON instead of Python json (faster, atomic writes)
- **Sandbox disabled** (`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`) — real CPU
  win on the i5-10210U; tradeoff noted, personal machine
- **Memory pressure settings** prevent the browser from pushing the system into
  swap-on-spinning-disk (the death scenario for responsiveness)

Measured: **~633 MB RSS vs ~777 MB for the Python version** (~150 MB less).

## Features

- Tabs (Ctrl+T/W, Alt+Left/Right), dark mode (forced via stylesheet + GTK theme)
- History deduped + atomic writes (crash-safe on HDD)
- Bookmarks
- Downloads → `~/Downloads`, collision-safe
- Window geometry persistence (dual-monitor aware)
- `--new-tab URL` single-instance IPC (Unix socket)
- No DRM: WebKitGTK ships no Widevine CDM and has no CDMi host plugin — engine
  fact, not a config issue. No Vivaldi hand-off exists in this build.

## WebKit engine optimization & the Widevine shim

The engine this webview talks to is ALSO rebuilt for the machine:
- `-march=native -O3` (replaces Arch's `-march=x86-64 -mtune=generic -O2`)
- Widevine **shim**: `webkit-shim/` contains a Chromium CDMi v10 host adapter
  (`CdmModule.{h,cpp}`) plus a `CDMFactoryWidevine` (a `com.widevine.alpha` partner
  of the in-tree Thunder/OpenCDM factory — `CDMWidevine.cpp`) and a GStreamer
  decryptor element (`WebKitWidevineDecryptorGStreamer.cpp`). The shim loads
  `libwidevinecdm.so` (found in `~/.config/vivaldi/WidevineCdm/`, Chromium's own
  CDM, no extra download) and implements the CDM host callbacks so Widevine
  sessions flow through WebKit's EME.

  Patch: `webkit-shim/acreetion-widevine.patch` (3.4k lines, 12 files) — apply with
  `patch -p2`. Rebuild: `~/bin/rebuild-webkit.sh install`. Honest limitation (v1):
  module-global HostSinks → last-opened session receives callbacks; persistent
  license store is stubbed.

## Build

```
cd ~/acreetion-surf
make           # cargo build --release → installs to ~/bin/acreetion-surf
make test      # smoke test: --help + 5s launch with window-up check

# engine rebuild (webkit2gtk + shim, ~90 min on i5-10210U):
~/bin/rebuild-webkit.sh install
~/bin/rebuild-webkit.sh bench      # EME probe through acreetion-surf
```

Paths: `~/.config/acreetion-surf/` (cookies, history, bookmarks, geometry),
`~/.local/share/acreetion-surf/` (WebKit site data), `~/.cache/acreetion-surf/`
(Web cache + IPC socket).