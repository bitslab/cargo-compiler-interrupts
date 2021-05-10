use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

use cargo_util::{paths, ProcessBuilder};
use color_eyre::eyre::{bail, eyre, WrapErr};
use tracing::{debug, info};

use crate::CIResult;

/// Drop the extra argument `name` in the arguments provided by `cargo`.
pub fn drop_name_args(name: &str) -> impl std::iter::Iterator<Item = String> + '_ {
    let mut found = false;
    std::env::args().filter(move |x| {
        if found {
            true
        } else {
            found = x == name;
            x != name
        }
    })
}

/// Initialize the logger.
pub fn init_logger(verbose: i32) {
    tracing::info!("initializing logger");
    let log_level = match verbose {
        0 => tracing::Level::ERROR,
        1 => tracing::Level::INFO,
        2 | _ => tracing::Level::DEBUG,
    };
    tracing_subscriber::fmt()
        .with_target(false)
        .with_timer(tracing_subscriber::fmt::time::uptime())
        .with_level(true)
        .with_max_level(log_level)
        .init();
}

/// Get the directory to the target folder, concatenate with the given path.
pub fn target_dir<P>(target: &Option<String>, release: &bool, path: P) -> CIResult<PathBuf>
where
    P: AsRef<Path>,
{
    let build_mode = if *release { "release" } else { "debug" };
    let path = path.as_ref();

    // get base target directory
    let mut target_dir = match std::env::var_os("CARGO_TARGET_DIR") {
        Some(dir) => dir.into(),
        None => std::env::current_dir()
            .wrap_err("failed to get current directory")?
            .join("target"),
    };

    // concatenate target if needed
    if let Some(target) = target {
        target_dir.push(target);
    }

    // concatenate compilation mode
    target_dir.push(build_mode);

    target_dir.push(path);

    if !target_dir.exists() {
        bail!("target directory does not exist: {}", target_dir.display());
    }

    debug!("target directory: {}", target_dir.display());
    Ok(target_dir)
}

/// Run `opt` with given arguments, input file and output file.
pub fn opt<P, I, S>(opt_args: I, stdin_path: P, stdout_path: P) -> CIResult<Output>
where
    P: AsRef<Path>,
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let stdin_path = stdin_path.as_ref();
    let stdout_path = stdout_path.as_ref();

    info!(
        "running opt: {}, {}",
        stdin_path
            .file_name()
            .ok_or_else(|| eyre!("failed to get file name"))?
            .to_string_lossy(),
        stdout_path
            .file_name()
            .ok_or_else(|| eyre!("failed to get file name"))?
            .to_string_lossy()
    );

    let mut cmd = Command::new("opt");
    cmd.args(opt_args);

    // stdin
    let stdin = fs::File::open(stdin_path)?;
    cmd.stdin(Into::<Stdio>::into(stdin));

    // stdout
    let stdout = fs::File::create(stdout_path)?;
    cmd.stdout(Into::<Stdio>::into(stdout));

    // execute
    let output = cmd.output().wrap_err("failed to execute `opt`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        bail!("`opt` returned failed status code");
    }

    Ok(output)
}

/// Run `llc` with input file.
pub fn llc<P>(input_file: P) -> CIResult<Output>
where
    P: AsRef<Path>,
{
    let input_file = input_file.as_ref();

    info!(
        "running llc: {}",
        input_file
            .file_name()
            .ok_or_else(|| eyre!("failed to get file name"))?
            .to_string_lossy()
    );

    let mut cmd = Command::new("llc");
    cmd.arg("-filetype=obj");
    cmd.arg(input_file);

    // execute
    let output = cmd.output().wrap_err("failed to execute `llc`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        bail!("`llc` returned failed status code");
    }

    Ok(output)
}

/// Run `cp` with source path and destination path.
pub fn cp<P>(src_path: P, dst_path: P) -> CIResult<Output>
where
    P: AsRef<Path>,
{
    let src_path = src_path.as_ref();
    let dst_path = dst_path.as_ref();

    info!(
        "running cp: {}, {}",
        src_path
            .file_name()
            .ok_or_else(|| eyre!("failed to get file name"))?
            .to_string_lossy(),
        dst_path
            .file_name()
            .ok_or_else(|| eyre!("failed to get file name"))?
            .to_string_lossy()
    );

    let mut cmd = Command::new("cp");
    cmd.arg(src_path);
    cmd.arg(dst_path);

    // execute
    let output = cmd.output().wrap_err("failed to execute `cp`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        bail!("`cp` returned failed status code");
    }

    Ok(output)
}

/// Scan the target directory for files matching the predicate.
pub fn scan_dir<Pa, Pr>(directory: Pa, predicate: Pr) -> CIResult<Vec<PathBuf>>
where
    Pa: AsRef<Path>,
    Pr: Fn(Option<&str>, Option<&str>) -> bool,
{
    let directory = directory.as_ref();
    debug!("scanning directory: {}", directory.display());
    let mut files = vec![];
    for entry in directory.read_dir()? {
        let entry = entry?;
        let path = entry.path();
        let file_stem = path.file_stem().and_then(|s| s.to_str());
        let extension = path.extension().and_then(|s| s.to_str());

        if predicate(file_stem, extension) {
            debug!("found path: {}", path.display());
            files.push(path);
        }
    }
    Ok(files)
}

/// Set the current directory to the root directory of the package.
pub fn set_current_dir_root() -> CIResult<()> {
    let mut dir = std::env::current_dir()?;
    dir.push("Cargo.toml");

    if !dir.exists() {
        info!("not running on the root directory of the package");
        let output = ProcessBuilder::new("cargo")
            .arg("locate-project")
            .arg("--message-format=plain")
            .exec_with_output()
            .wrap_err("failed to execute cargo locate-project")?;
        let stdout = String::from_utf8(output.stdout)?;
        let path_toml = Path::new(&stdout);
        let root_dir = path_toml.parent().unwrap();
        std::env::set_current_dir(root_dir)?;
        info!("set current directory: {}", root_dir.display());
    }

    Ok(())
}

/// Get the configuration path.
pub fn config_path() -> CIResult<PathBuf> {
    let mut path = dirs::config_dir().ok_or_else(|| eyre!("failed to get config dir"))?;
    path.push("cargo-ci");
    if !path.exists() {
        paths::create_dir_all(&path)?;
    }
    Ok(path)
}
