use anyhow::{bail, Context, Result};
use cargo_util::{paths, ProcessBuilder};

static CI_CPP_PATH: &str = "src/libci/CompilerInterrupt.cpp";

fn main() -> Result<()> {
    println!("cargo:rerun-if-changed={}", CI_CPP_PATH);
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=Cargo.lock");

    let output = ProcessBuilder::new("rustc")
        .arg("-vV")
        .exec_with_output()
        .context("failed to execute `rustc -vV`")?;
    let rustc_output = String::from_utf8(output.stdout)?;
    let rustc_llvm_version = rustc_output
        .lines()
        .filter_map(|line| line.strip_prefix("LLVM version: "))
        .next()
        .expect("rustc version should have LLVM version field")
        .to_string();
    let rustc_llvm_version = rustc_llvm_version.trim();

    let output = ProcessBuilder::new("llvm-config")
        .arg("--version")
        .exec_with_output()
        .context("failed to execute `llvm-config --version`")?;
    let llvm_version = String::from_utf8(output.stdout)?;
    let llvm_version = llvm_version.trim();

    if llvm_version != rustc_llvm_version {
        bail!(
            "LLVM version from Rust toolchain ({}) does not \
            match with LLVM version from LLVM toolchain ({})",
            rustc_llvm_version,
            llvm_version
        );
    }

    let output = ProcessBuilder::new("llvm-config")
        .arg("--cxxflags")
        .exec_with_output()
        .context("failed to execute `llvm-config --cxxflags`")?;
    let cxx_flags = String::from_utf8(output.stdout)?;

    let output = ProcessBuilder::new("llvm-config")
        .arg("--ldflags")
        .exec_with_output()
        .context("failed to execute `llvm-config --ldflags`")?;
    let ld_flags = String::from_utf8(output.stdout)?;

    let common_flags = "-O3 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC";

    let so_flags = match std::env::consts::OS {
        "macos" => "-bundle -undefined dynamic_lookup",
        _ => "-shared",
    };

    let cargo_lib = format!("{}/lib", std::env::var("CARGO_HOME")?);
    if !std::path::PathBuf::from(&cargo_lib).exists() {
        paths::create_dir_all(&cargo_lib)?;
    }

    let mut cpp = ProcessBuilder::new("c++");
    cpp.arg(CI_CPP_PATH);
    cpp.args(&[
        "-o".to_string(),
        format!("{}/libcompilerinterrupt.so", cargo_lib),
    ]);
    cpp.args(so_flags.split_ascii_whitespace());
    cpp.args(cxx_flags.split_ascii_whitespace());
    cpp.args(ld_flags.split_ascii_whitespace());
    cpp.args(common_flags.split_ascii_whitespace());

    cpp.exec().context("failed to execute `c++`")?;

    Ok(())
}
