//! Implementation of `cargo-run-ci`.

use anyhow::bail;
use cargo_util::ProcessBuilder;
use faccess::PathExt;

use crate::error::CIError;
use crate::opts::RunOpts;
use crate::{util, CIResult};

/// Main routine for `cargo-run-ci`.
pub fn exec(opts: RunOpts) -> CIResult<()> {
    let deps_path = util::target_path(&opts.target, &opts.release)?;
    let binary_paths = util::scan_path(&deps_path, |path| {
        path.executable() && path.is_file() && util::file_stem(path).contains("-ci")
    })?;
    if binary_paths.is_empty() {
        bail!(CIError::BinaryNotFound);
    }

    let binary_names = binary_paths
        .iter()
        .map(util::file_stem)
        .map(|mut path| {
            remove_ci(&mut path);
            path
        })
        .collect::<Vec<_>>()
        .join(", ");

    if let Some(binary_name) = opts.bin {
        for path in binary_paths {
            let mut name = util::file_name(&path);
            remove_ci(&mut name);
            if binary_name == name {
                return ProcessBuilder::new(&path)
                    .args(&opts.args.unwrap_or_default())
                    .exec_replace();
            }
        }

        bail!(CIError::BinaryNotAvailable(binary_name, binary_names));
    } else if binary_paths.len() == 1 {
        return ProcessBuilder::new(&binary_paths[0])
            .args(&opts.args.unwrap_or_default())
            .exec_replace();
    }

    bail!(CIError::BinaryNotDetermine(binary_names));
}

/// Remove suffix "-ci" from the given string.
fn remove_ci(s: &mut String) {
    s.pop();
    s.pop();
    s.pop();
}
