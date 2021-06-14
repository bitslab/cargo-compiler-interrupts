use anyhow::bail;
use cargo_util::ProcessBuilder;
use faccess::PathExt;

use crate::args::RunArgs;
use crate::error::CIError;
use crate::{util, CIResult};

pub fn exec(args: RunArgs) -> CIResult<()> {
    let deps_dir = util::target_dir(&args.target, &args.release)?;
    let binary_paths = util::scan_dir(&deps_dir, |path| {
        path.executable() && path.is_file() && util::file_stem_unwrapped(path).contains("-ci")
    })?;
    if binary_paths.is_empty() {
        bail!(CIError::BinaryNotFound);
    }

    let binary_names = binary_paths
        .iter()
        .map(|path| util::file_stem_unwrapped(path))
        .map(|mut path| {
            remove_ci(&mut path);
            path
        })
        .collect::<Vec<_>>()
        .join(", ");

    if let Some(binary_name) = args.bin {
        for path in binary_paths {
            let mut name = util::file_name_unwrapped(&path);
            remove_ci(&mut name);
            if binary_name == name {
                return ProcessBuilder::new(&path).exec_replace();
            }
        }

        bail!(CIError::BinaryNotAvailable(binary_name, binary_names));
    } else if binary_paths.len() == 1 {
        return ProcessBuilder::new(&binary_paths[0]).exec_replace();
    }

    bail!(CIError::BinaryNotDetermine(binary_names));
}

fn remove_ci(s: &mut String) {
    // remove "-ci"
    s.pop();
    s.pop();
    s.pop();
}
