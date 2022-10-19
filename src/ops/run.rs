//! Implementation of `cargo-run-ci`.

use anyhow::{bail, Context};
use cargo_util::ProcessBuilder;
use clap::Parser;
use std::path::PathBuf;

use crate::args::RunArgs;
use crate::error::Error;
use crate::paths::PathExt;
use crate::{cargo, util, CIResult, RUN_CI_BIN_NAME};

/// Main routine for `cargo-run-ci`.
pub fn exec() -> CIResult<()> {
    let args = if std::env::args().next().unwrap_or_default() == RUN_CI_BIN_NAME {
        RunArgs::parse()
    } else {
        RunArgs::parse_from(std::env::args().skip(1))
    };

    util::init_logger(&args.log_level)?;
    util::set_current_workspace_root_dir().context("failed to set the root directory")?;

    _exec(args)
}

/// Core routine for `cargo-run-ci`.
fn _exec(args: RunArgs) -> CIResult<()> {
    let mut cargo = cargo::Cargo::with_args(args.cargo_args);
    cargo.build()?;

    let binaries = cargo.target_dir.read_dir(|path| path.executable())?;

    let (integrates, originals): (Vec<PathBuf>, _) = binaries
        .into_iter()
        .partition(|binary| binary.file_stem().unwrap_or_default().contains("-ci"));

    if originals.is_empty() {
        bail!(Error::BinaryNotFound);
    }

    if integrates.is_empty() {
        bail!(Error::IntegratedBinaryNotFound);
    }

    let names = originals
        .iter()
        .map(|p| p.file_stem())
        .filter_map(|p| p.ok())
        .collect::<Vec<_>>()
        .join(", ");

    if let Some(binary_name) = args.binary_name {
        for (integrated, original) in integrates.iter().zip(originals.iter()) {
            if binary_name == original.file_name()? {
                return ProcessBuilder::new(integrated)
                    .args(&args.binary_args)
                    .exec_replace();
            }
        }

        bail!(Error::BinaryNotAvailable(binary_name, names));
    } else if integrates.len() == 1 {
        return ProcessBuilder::new(&integrates[0])
            .args(&args.binary_args)
            .exec_replace();
    }

    bail!(Error::BinaryNotDetermine(names));
}
