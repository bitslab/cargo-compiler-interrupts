use anyhow::Context;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use tracing::{debug, info};

use crate::args;
use crate::CIResult;

/// Get the directory to the target folder, concatenate with the given path.
pub(crate) fn target_dir<P>(path: P) -> CIResult<PathBuf>
where
    P: AsRef<Path>,
{
    let path = path.as_ref();
    let args = args::get();

    // get base target directory
    let mut target_dir = match std::env::var_os("CARGO_TARGET_DIR") {
        Some(dir) => dir.into(),
        None => std::env::current_dir()
            .context("failed to get current directory")?
            .join("target"),
    };

    // concatenate target if needed
    if let Some(target) = &args.target {
        target_dir.push(target);
    }

    // concatenate compilation mode
    target_dir.push(&args.build_mode);

    target_dir.push(path);

    if !target_dir.exists() {
        anyhow::bail!("target directory does not exist: {}", target_dir.display());
    }

    debug!("target directory: {}", target_dir.display());
    Ok(target_dir)
}

/// Run `opt` with given arguments, input file and output file.
pub(crate) fn opt<P, I, S>(args: I, stdin_path: P, stdout_path: P) -> CIResult<Output>
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
            .context("failed to get file name")?
            .to_string_lossy(),
        stdout_path
            .file_name()
            .context("failed to get file name")?
            .to_string_lossy()
    );

    let mut cmd = Command::new("opt");
    cmd.args(args);

    // stdin
    let stdin = fs::File::open(stdin_path)?;
    cmd.stdin(Into::<Stdio>::into(stdin));

    // stdout
    let stdout = fs::File::create(stdout_path)?;
    cmd.stdout(Into::<Stdio>::into(stdout));

    // execute
    let output = cmd.output().context("failed to execute `opt`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        anyhow::bail!("`opt` returned failed status code");
    }

    Ok(output)
}

/// Run `llc` with input file.
pub(crate) fn llc<P>(input_file: P) -> CIResult<Output>
where
    P: AsRef<Path>,
{
    let input_file = input_file.as_ref();

    info!(
        "running llc: {}",
        input_file
            .file_name()
            .context("failed to get file name")?
            .to_string_lossy()
    );

    let mut cmd = Command::new("llc");
    cmd.arg("-filetype=obj");
    cmd.arg(input_file);

    // execute
    let output = cmd.output().context("failed to execute `llc`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        anyhow::bail!("`llc` returned failed status code");
    }

    Ok(output)
}

/// Run `cp` with source path and destination patht.
pub(crate) fn cp<P>(src_path: P, dst_path: P) -> CIResult<Output>
where
    P: AsRef<Path>,
{
    let src_path = src_path.as_ref();
    let dst_path = dst_path.as_ref();

    info!(
        "running cp: {}, {}",
        src_path
            .file_name()
            .context("failed to get file name")?
            .to_string_lossy(),
        dst_path
            .file_name()
            .context("failed to get file name")?
            .to_string_lossy()
    );

    let mut cmd = Command::new("cp");
    cmd.arg(src_path);
    cmd.arg(dst_path);

    // execute
    let output = cmd.output().context("failed to execute `cp`")?;

    if !output.status.success() {
        info!("status code: {:?}", output.status);
        info!("stdout: {}", String::from_utf8_lossy(&output.stdout));
        info!("stderr: {}", String::from_utf8_lossy(&output.stderr));
        anyhow::bail!("`cp` returned failed status code");
    }

    Ok(output)
}

/// Scan the target directory for files matching the predicate.
pub(crate) fn scan_dir<Pa, Pr>(directory: Pa, predicate: Pr) -> CIResult<Vec<PathBuf>>
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

/// Initialize the logger
pub(crate) fn init_logger() {
    info!("initializing logger");
    let log_level = match args::get().verbose {
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
