// build.rs — compile the C browser shell and link GTK/WebKit/json-glib
use std::process::Command;
use std::path::Path;

fn pkg_config(args: &[&str]) -> Vec<String> {
    let out = Command::new("pkg-config")
        .args(args)
        .output()
        .expect("pkg-config not found");
    if !out.status.success() {
        panic!(
            "pkg-config failed for {:?}: {}",
            args,
            String::from_utf8_lossy(&out.stderr)
        );
    }
    String::from_utf8(out.stdout)
        .unwrap()
        .trim()
        .split_whitespace()
        .map(String::from)
        .collect()
}

fn main() {
    let pkgs = ["gtk+-3.0", "webkit2gtk-4.1", "json-glib-1.0"];

    // C compiler flags (include paths + -pthread etc.)
    let mut cflag_args = vec!["--cflags"];
    cflag_args.extend(&pkgs);
    let cflags = pkg_config(&cflag_args);

    // Linker flags
    let mut lib_args = vec!["--libs"];
    lib_args.extend(&pkgs);
    let libs = pkg_config(&lib_args);

    // Check for Firefox addon adapter
    let mut build = cc::Build::new();
    build.file("src/surf.c");
    for flag in &cflags {
        build.flag(flag);
    }
    
    // Try to find adapter
    let adapter_paths = [
        "third_party/firefox-adapter/include",
        "../acreetion-surf-webkit-to-firefox-addon-adapter/include",
        "/usr/local/include/acrefxadapter",
        "/usr/include/acrefxadapter",
    ];
    let mut found_adapter = false;
    for path in &adapter_paths {
        if Path::new(path).exists() {
            build.include(path);
            found_adapter = true;
            break;
        }
    }
    
    if found_adapter {
        build.define("HAVE_FIREFOX_ADDONS", None);
    }
    
    // Silence GObject callback unused-parameter noise (required by signal signatures)
    build.flag("-Wno-unused-parameter");
    build.flag("-Wno-unused-function");
    build.flag("-Wno-unused-variable");
    build.compile("surf_c");

    // Tell cargo to link the shared libs
    for lib in &libs {
        if let Some(name) = lib.strip_prefix("-l") {
            println!("cargo:rustc-link-lib=dylib={}", name);
        } else if lib == "-pthread" {
            println!("cargo:rustc-link-lib=dylib=pthread");
        } else if let Some(path) = lib.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={}", path);
        }
    }
    
    // Link Firefox addon adapter if found
    let lib_paths = [
        "third_party/firefox-adapter/lib",
        "third_party/firefox-adapter/build",
        "../acreetion-surf-webkit-to-firefox-addon-adapter/build",
        "/usr/local/lib",
        "/usr/lib",
    ];
    for path in &lib_paths {
        if Path::new(path).join("libacrefxadapter.so").exists() {
            println!("cargo:rustc-link-search=native={}", path);
            println!("cargo:rustc-link-lib=dylib=acrefxadapter");
            break;
        }
    }

    println!("cargo:rerun-if-changed=src/surf.c");
    println!("cargo:rerun-if-changed=third_party/firefox-adapter/");
}
