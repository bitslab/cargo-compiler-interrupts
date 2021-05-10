use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

use cargo_util::ProcessBuilder;
use color_eyre::eyre::{eyre, WrapErr};
use color_eyre::Section;

use crate::args::RunArgs;
use crate::{util, CIResult};

pub fn exec(args: RunArgs) -> CIResult<()> {
    let deps_dir = util::target_dir(&args.target, &args.release, "deps")
        .wrap_err("Failed to locate the target folder")
        .suggestion("Try run `cargo build-ci` first")?;

    let binary_paths = {
        let v = util::scan_dir(&deps_dir, |_, extension| extension == None)?;
        v.into_iter()
            .filter(|path| is_executable(path))
            .collect::<Vec<PathBuf>>()
    };
    let binary_names = binary_paths
        .iter()
        .map(|path| {
            path.file_name()
                .unwrap()
                .to_os_string()
                .into_string()
                .unwrap()
        })
        .collect::<Vec<String>>()
        .join(", ");

    if let Some(binary_name) = args.bin {
        for path in binary_paths {
            let name = path
                .file_name()
                .ok_or_else(|| eyre!("failed to get file name"))?;
            if name == OsStr::new(&binary_name) {
                return ProcessBuilder::new(path).exec_replace();
            }
        }

        return Err(eyre!("Failed to execute the binary '{}'", binary_name)
            .suggestion(format!("Available binaries: {}", binary_names)));
    } else {
        if binary_paths.len() == 1 {
            return ProcessBuilder::new(&binary_paths[0]).exec_replace();
        } else {
            return Err(
                eyre!("Could not determine which binary to run").suggestion(format!(
                    "Run `cargo run-ci --bin` to specify a binary\nAvailable binaries: {}",
                    binary_names
                )),
            );
        }
    }
}

#[cfg(unix)]
fn is_executable<P: AsRef<Path>>(path: P) -> bool {
    use std::os::unix::prelude::*;
    fs::metadata(path)
        .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

#[cfg(windows)]
fn is_executable<P: AsRef<Path>>(path: P) -> bool {
    path.as_ref().is_file()
}
