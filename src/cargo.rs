use std::process::Output;

use anyhow::Context;
use cargo_metadata::{Metadata, MetadataCommand};
use cargo_util::ProcessBuilder;
use tracing::{debug, info};

use crate::args::BuildArgs;
use crate::CIResult;

/// Run `cargo build`.
pub fn build(args: &BuildArgs) -> CIResult<Output> {
    info!("running cargo build");

    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("build");

    // release mode
    if args.release {
        cmd.arg("--release");
    }

    // target
    if let Some(target) = &args.target {
        cmd.arg("--target");
        cmd.arg(target);
    }

    // print the internal linker invocation
    cmd.env("RUSTC_LOG", "rustc_codegen_ssa::back::link=info");

    // TODO: cargo uses RUSTFLAGS first, hence overriding flags in config.toml
    // find an alternative way to respect end-user's rustc flags
    // https://doc.rust-lang.org/cargo/reference/config.html#buildrustflags
    // moreover, adding external flags will trigger full re-compilation
    // when end-user executes normal `cargo build`

    // `--emit=llvm-ir` to emit LLVM IR bitcode
    // `-C save-temps` to save temporary files during the compilation
    // `-C passes` to pass extra LLVM passes to the compilation
    // https://doc.rust-lang.org/rustc/codegen-options/index.html

    // for some reason `env` does not escape quote in string literal...
    let rustflags = [
        "--emit=llvm-ir",
        "-Csave-temps",
        "-Cpasses=postdomtree",
        "-Cpasses=mem2reg",
        "-Cpasses=indvars",
        "-Cpasses=loop-simplify",
        "-Cpasses=branch-prob",
        "-Cpasses=scalar-evolution",
    ];
    cmd.env("RUSTFLAGS", rustflags.join(" "));

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
    let metadata = cmd.exec().context("failed to execute `cargo metadata`")?;
    Ok(metadata)
}
