// build.rs — compile the C browser shell and link GTK/WebKit/json-glib
use std::process::Command;

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

    // Compile the C shell
    let mut build = cc::Build::new();
    build.file("src/surf.c");
    for flag in &cflags {
        build.flag(flag);
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

    println!("cargo:rerun-if-changed=src/surf.c");
}
// Firefox addon adapter support
fn main_ext() {}
