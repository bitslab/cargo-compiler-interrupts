//! Miscellaneous utilities.

use std::str::FromStr;

use anyhow::Context;
use tracing::{debug, info, Level};
use tracing_subscriber::util::SubscriberInitExt;

use crate::{cargo, CIResult};

/// Initializes the logger.
pub fn init_logger(level: &String) -> CIResult<()> {
    info!("initializing logger with log level: {}", level);

    let level = Level::from_str(level)?;

    let builder = tracing_subscriber::fmt()
        .with_target(false)
        .with_level(true)
        .with_max_level(level);

    if level <= Level::WARN {
        builder
            .without_time()
            .finish()
            .try_init()
            .context("failed to initialize the logger")?;
    } else {
        builder
            .with_timer(tracing_subscriber::fmt::time::uptime())
            .finish()
            .try_init()
            .context("failed to initialize the logger")?;
    }

    Ok(())
}

/// Sets the current directory to the root directory of the workspace.
pub fn set_current_workspace_root_dir() -> CIResult<()> {
    let root_dir = cargo::locate_project()?;
    let current_dir = std::env::current_dir()?;
    debug!(?root_dir);
    debug!(?current_dir);
    if current_dir != root_dir {
        info!("not running on the root directory of the package");
        info!("set current working directory to: {}", root_dir.display());
        std::env::set_current_dir(&root_dir)?;
    }

    Ok(())
}

/// Gets a human readable String for Duration.
pub fn human_duration(duration: std::time::Duration) -> String {
    let secs = duration.as_secs();
    if secs >= 60 {
        format!("{}m {:02}s", secs / 60, secs % 60)
    } else {
        format!("{}.{:02}s", secs, duration.subsec_nanos() / 10_000_000)
    }
}
