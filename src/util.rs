use std::path::{Path, PathBuf};

use anyhow::Context;
use cargo_util::{paths, ProcessBuilder};
use tracing::{debug, info};

use crate::CIResult;

/// Initialize the logger.
pub fn init_logger(verbose: i32) {
    tracing::info!("initializing logger");
    let log_level = match verbose {
        0 => tracing::Level::ERROR,
        1 => tracing::Level::INFO,
        _ => tracing::Level::DEBUG,
    };
    tracing_subscriber::fmt()
        .with_target(false)
        .with_timer(tracing_subscriber::fmt::time::uptime())
        .with_level(true)
        .with_max_level(log_level)
        .init();
}

/// Get the root directory of the package.
fn package_root_dir() -> CIResult<PathBuf> {
    let output = ProcessBuilder::new("cargo")
        .arg("locate-project")
        .arg("--message-format=plain")
        .exec_with_output()?;
    let stdout = String::from_utf8(output.stdout)?;
    let path_toml = Path::new(&stdout);
    let root_dir = path_toml.parent().context("failed to get parent dir")?;

    Ok(root_dir.to_path_buf())
}

/// Set the current directory to the root directory of the package.
pub fn set_current_package_root_dir() -> CIResult<()> {
    let mut dir = std::env::current_dir()?;
    dir.push("Cargo.toml");

    if !dir.is_file() {
        info!("not running on the root directory of the package");
        let root_dir = package_root_dir()?;
        std::env::set_current_dir(&root_dir)?;
        info!("set current directory: {}", root_dir.display());
    }

    Ok(())
}

/// Get the path to the configuration directory.
pub fn config_path() -> CIResult<PathBuf> {
    let mut path = dirs::config_dir().context("failed to get config dir")?;
    path.push("cargo-compiler-interrupts");
    if !path.exists() {
        paths::create_dir_all(&path)?;
    }
    Ok(path)
}

/// Get the directory to the target folder and concatenate with the given path.
pub fn target_dir(target: &Option<String>, release: &bool) -> CIResult<PathBuf> {
    // get base target directory
    let mut dir = match std::env::var_os("CARGO_TARGET_DIR") {
        Some(dir) => dir.into(),
        None => package_root_dir()?.join("target"),
    };

    if let Some(target) = target {
        dir.push(target);
    }

    let build_mode = if *release { "release" } else { "debug" };
    dir.push(build_mode);

    debug!("target directory: {}", dir.display());
    Ok(dir)
}

/// Scan the target directory for files matching the predicate.
pub fn scan_dir<P: AsRef<Path>>(
    directory: P,
    predicate: fn(PathBuf) -> bool,
) -> CIResult<Vec<PathBuf>> {
    let directory = directory.as_ref();
    debug!("scanning directory: {}", directory.display());
    let mut files = vec![];
    for entry in directory.read_dir()? {
        let entry = entry?;
        let path = entry.path();
        if predicate(path.clone()) {
            files.push(path);
        }
    }
    Ok(files)
}

/// Append the suffix to the file stem of a path.
pub fn append_suffix<P: AsRef<Path>>(path: P, suffix: &str) -> PathBuf {
    let path = path.as_ref();
    let file_stem = file_stem_unwrapped(path);
    let extension = extension(path);
    let file_name = match extension {
        Some(extension) => format!("{}-{}.{}", file_stem, suffix, extension),
        None => format!("{}-{}", file_stem, suffix),
    };
    path.with_file_name(file_name)
}

/// Get the file stem of a path and unwrapped by default.
pub fn file_stem_unwrapped<P: AsRef<Path>>(path: P) -> String {
    let path = path.as_ref();
    path.file_stem()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
        .unwrap_or_default()
}

/// Get the file name of a path and unwrapped by default.
pub fn file_name_unwrapped<P: AsRef<Path>>(path: P) -> String {
    let path = path.as_ref();
    path.file_name()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
        .unwrap_or_default()
}

/// Get the file extension of a path.
fn extension<P: AsRef<Path>>(path: P) -> Option<String> {
    let path = path.as_ref();
    path.extension()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
}

/// Get the file extension of a path and unwrapped by default.
pub fn extension_unwrapped<P: AsRef<Path>>(path: P) -> String {
    extension(path).unwrap_or_default()
}

/// Sanity check for LLVM toolchain and its binaries.
pub fn llvm_toolchain(binaries: &mut Vec<String>) -> CIResult<String> {
    use crate::error::CIError::*;
    use anyhow::bail;

    // get rustc's llvm version
    let output = ProcessBuilder::new("rustc").arg("-vV").exec_with_output()?;
    let rustc_output = String::from_utf8(output.stdout)?;
    let rustc_ver = rustc_output
        .lines()
        .filter_map(|line| line.strip_prefix("LLVM version: "))
        .next()
        .expect("`rustc -vV` should have the LLVM version field")
        .trim()
        .to_string();
    let major_ver = rustc_ver.split(".").next().unwrap();

    // get llvm version from both binaries with and without version suffix
    let cfg = ProcessBuilder::new("llvm-config")
        .arg("--version")
        .exec_with_output();
    let cfg_sf = ProcessBuilder::new(format!("llvm-config-{}", major_ver))
        .arg("--version")
        .exec_with_output();

    // check if rustc and llvm are compatible and add version suffix if needed
    let add_sf = match (cfg, cfg_sf) {
        (Ok(o), Ok(o_sf)) => {
            let ver = String::from_utf8(o.stdout)?.trim().to_string();
            let ver_sf = String::from_utf8(o_sf.stdout)?.trim().to_string();
            if rustc_ver == ver {
                false
            } else if rustc_ver == ver_sf {
                true
            } else {
                bail!(LLVMVersionNotMatch(rustc_ver, ver_sf));
            }
        }
        (Ok(o), Err(_)) => {
            let ver = String::from_utf8(o.stdout)?.trim().to_string();
            if rustc_ver != ver {
                bail!(LLVMVersionNotMatch(rustc_ver, ver));
            }
            false
        }
        (Err(_), Ok(o_sf)) => {
            let ver_sf = String::from_utf8(o_sf.stdout)?.trim().to_string();
            if rustc_ver != ver_sf {
                bail!(LLVMVersionNotMatch(rustc_ver, ver_sf));
            }
            true
        }
        (Err(_), Err(_)) => {
            bail!(LLVMNotInstalled);
        }
    };

    // add version suffix if needed to llvm's binaries
    for binary in binaries {
        *binary = if add_sf {
            format!("{}-{}", binary, major_ver)
        } else {
            binary.to_string()
        }
    }

    Ok(rustc_ver)
}
