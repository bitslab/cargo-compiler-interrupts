//! Handles configuration for the Compiler Interrupts library.

use anyhow::Context;
use cargo_util::paths;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use tracing::{debug, warn};

use crate::paths::PathExt;
use crate::CIResult;

/// Configuration for the Compiler Interrupts library.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Config {
    /// Path to the library.
    pub library_path: PathBuf,
    /// Path to the debug-enabled library.
    pub library_debug_path: PathBuf,
    /// Arguments for the library.
    pub library_args: Vec<String>,
    /// LLVM version used to compile the library.
    pub llvm_version: String,
    /// Checksum of the source code.
    pub checksum: String,
    /// Remote URL for the source code.
    pub url: String,
}

impl Config {
    /// Loads the configuration.
    pub fn load() -> CIResult<Self> {
        let default = Self::default();
        let mut path = Config::dir()?;
        path.push("default.cfg");
        let file = match paths::read(&path) {
            Ok(file) => file,
            Err(error) => {
                Self::save(&default).context("failed to save default config")?;

                warn!("config file not found, use default config");
                debug!(?error);

                return Ok(default);
            }
        };
        match toml::from_str(&file) {
            Ok(config) => Ok(config),
            Err(error) => {
                let old_path = path.append_suffix("old")?;
                paths::copy(&path, &old_path).context("failed to backup the old config")?;
                Self::save(&default)?;

                warn!("found incompatible config file, replaced with default config");
                warn!("old config file can be found at: {}", old_path.display());
                debug!(?error);

                Self::load()
            }
        }
    }

    /// Saves the configuration.
    pub fn save(config: &Self) -> CIResult<()> {
        let mut path = Config::dir()?;
        path.push("default.cfg");
        let s = toml::to_string_pretty(config).context("failed to parse the config")?;
        paths::write(path, s).context("failed to save the config")
    }

    /// Gets the configuration directory.
    pub fn dir() -> CIResult<PathBuf> {
        let mut path = dirs::config_dir().context("failed to get the config directory")?;
        path.push("cargo-compiler-interrupts");
        paths::create_dir_all(&path)?;
        Ok(path)
    }
}
