//! Miscellaneous utilities.

use std::path::{Path, PathBuf};

use anyhow::{bail, Context};
use cargo_util::{paths, ProcessBuilder};
use tracing::{debug, info};

use crate::error::CIError::*;
use crate::CIResult;

/// Initializes the logger.
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

/// Gets the root directory of the package.
fn package_root_dir() -> CIResult<PathBuf> {
    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("locate-project");
    cmd.arg("--message-format=plain");
    cmd.env("CARGO_TERM_COLOR", "always");
    let output = cmd.exec_with_output()?;
    let stdout = String::from_utf8(output.stdout)?;
    let path_toml = Path::new(&stdout);
    let root_dir = path_toml.parent().context("failed to get parent dir")?;

    Ok(root_dir.to_path_buf())
}

/// Sets the current directory to the root directory of the package.
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

/// Gets the path to the configuration directory.
pub fn config_path() -> CIResult<PathBuf> {
    let mut path = dirs::config_dir().context("failed to get config dir")?;
    path.push("cargo-compiler-interrupts");
    if !path.exists() {
        paths::create_dir_all(&path)?;
    }
    Ok(path)
}

/// Gets the path to the target directory.
pub fn target_path(target: &Option<String>, release: &bool) -> CIResult<PathBuf> {
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

/// Scans the target directory for files matching the predicate.
pub fn scan_path<P: AsRef<Path>>(
    path: P,
    predicate: fn(PathBuf) -> bool,
) -> CIResult<Vec<PathBuf>> {
    let path = path.as_ref();
    debug!("scanning path: {}", path.display());
    let mut files = Vec::new();
    for entry in path.read_dir()? {
        let entry = entry?;
        let path = entry.path();
        if predicate(path.clone()) {
            files.push(path);
        }
    }
    Ok(files)
}

/// Appends the suffix to the file stem of a path.
pub fn append_suffix<P: AsRef<Path>>(path: P, suffix: &str) -> PathBuf {
    let path = path.as_ref();
    let file_stem = file_stem(path);
    let extension = _extension(path);
    let file_name = if let Some(extension) = extension {
        format!("{}-{}.{}", file_stem, suffix, extension)
    } else {
        format!("{}-{}", file_stem, suffix)
    };
    path.with_file_name(file_name)
}

/// Gets the file stem of a path. Empty string is returned if path was not valid UTF-8.
pub fn file_stem<P: AsRef<Path>>(path: P) -> String {
    let path = path.as_ref();
    path.file_stem()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
        .unwrap_or_default()
}

/// Gets the file name of a path. Empty string is returned if path was not valid UTF-8.
pub fn file_name<P: AsRef<Path>>(path: P) -> String {
    let path = path.as_ref();
    path.file_name()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
        .unwrap_or_default()
}

/// Get the file extension of a path. Empty string is returned if path was not valid UTF-8.
pub fn extension<P: AsRef<Path>>(path: P) -> String {
    _extension(path).unwrap_or_default()
}

/// Gets the file extension of a path.
fn _extension<P: AsRef<Path>>(path: P) -> Option<String> {
    let path = path.as_ref();
    path.extension()
        .map(std::ffi::OsStr::to_string_lossy)
        .map(|e| e.to_string())
}

/// Converts a path to string. Empty string is returned if path was not valid UTF-8.
pub fn path_to_string<P: AsRef<Path>>(path: P) -> String {
    let path = path.as_ref();
    path.to_str().unwrap_or_default().to_string()
}

/// Determines appropriate version for LLVM toolchain and its binaries.
pub fn llvm_toolchain(binaries: &mut Vec<String>) -> CIResult<String> {
    let llvm_min_version_supported: i32 = 9;

    // get llvm version from rustc
    let output = ProcessBuilder::new("rustc").arg("-vV").exec_with_output()?;
    let rustc_output = String::from_utf8(output.stdout)?;
    let rustc_ver = rustc_output
        .lines()
        .find_map(|line| line.strip_prefix("LLVM version: "))
        .context("`rustc -vV` should have the LLVM version field")?
        .trim()
        .to_string();
    let major_ver = rustc_ver
        .split('.')
        .next()
        .context("`rustc` llvm version is not valid")?;
    let major_ver_i32 = major_ver
        .parse::<i32>()
        .context("`rustc` llvm version is not valid")?;

    // check if llvm version is supported
    if major_ver_i32 < llvm_min_version_supported {
        bail!(LLVMNotSupported(
            major_ver.to_string(),
            llvm_min_version_supported.to_string()
        ));
    }

    // get llvm version from `llvm-config` with and without version suffix
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

    // add version suffix if needed to llvm binaries
    for binary in binaries {
        *binary = if add_sf {
            format!("{}-{}", binary, major_ver)
        } else {
            (*binary).to_string()
        }
    }

    Ok(rustc_ver)
}

/// Gets human readable for duration.
pub fn human_duration(duration: std::time::Duration) -> String {
    let secs = duration.as_secs();

    if secs >= 60 {
        format!("{}m {:02}s", secs / 60, secs % 60)
    } else {
        format!("{}.{:02}s", secs, duration.subsec_nanos() / 10_000_000)
    }
}
