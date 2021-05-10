use std::process::Output;

use cargo_metadata::{Metadata, MetadataCommand};
use cargo_util::ProcessBuilder;
use color_eyre::eyre::WrapErr;
use tracing::{debug, info};

use crate::args::BuildArgs;
use crate::CIResult;

/// Run `cargo build`.
pub fn build(args: &BuildArgs) -> CIResult<Output> {
    info!("running cargo build");

    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("build");
    cmd.arg("-vv");

    // release mode
    if args.release {
        cmd.arg("--release");
    }

    // target
    if let Some(target) = &args.target {
        cmd.arg("--target");
        cmd.arg(target);
    }

    // RUSTC
    if let Ok(v) = std::env::var("RUSTC") {
        cmd.env("RUSTC", v);
    }

    // RUSTFLAGS
    let rustflags = std::env::var("RUSTFLAGS").unwrap_or_default();

    // TODO: respect RUSTFLAGS from .cargo/config.toml
    // `--emit=llvm-ir` to emit LLVM IR bitcode
    // `-C debuginfo=0` to not pollute the bitcode with debug symbols
    // `-C save-temps` to save temporary files during the compilation
    // `-Z print-link-args` to print the internal linker command
    let extra_rustflags = "--emit=llvm-ir -C debuginfo=0 -C save-temps -Z print-link-args";
    cmd.env("RUSTFLAGS", format!("{} {}", rustflags, extra_rustflags));

    debug!("args: {:?}", cmd.get_args());
    debug!("envs: {:?}", cmd.get_envs());
    cmd.exec_with_output()
}

/// Run `cargo clean`.
pub fn clean() -> CIResult<Output> {
    info!("running cargo clean");
    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("clean");

    cmd.exec_with_output()
}

/// Run `cargo metadata`.
pub fn metadata() -> CIResult<Metadata> {
    info!("running cargo metadata");
    let mut cmd = MetadataCommand::new();
    cmd.no_deps();
    let metadata = cmd.exec().wrap_err("failed to execute `cargo metadata`")?;

    Ok(metadata)
}
