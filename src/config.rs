//! Handles configuration for the Compiler Interrupts library.

use cargo_util::paths;
use serde::{Deserialize, Serialize};
use tracing::{debug, info};

use crate::{util, CIResult};

/// Configuration for the Compiler Interrupts library.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Config {
    /// Path to the library.
    pub library_path: String,

    /// Path to the debug-enabled library.
    pub library_path_dbg: String,

    /// LLVM version used to compile the library.
    pub llvm_version: String,

    /// Default arguments.
    pub default_args: Vec<String>,

    /// Checksum of the source code.
    pub checksum: String,

    /// Remote URL for the source code.
    pub url: String,
}

impl Config {
    /// Load the configuration.
    pub fn load() -> CIResult<Config> {
        let mut path = util::config_path()?;
        path.push("default.cfg");
        let file = match paths::read(&path) {
            Ok(file) => file,
            Err(e) => {
                info!("config file not available, default config loaded");
                debug!("error: {}", e);
                return Ok(Config::default());
            }
        };
        match toml::from_str(&file) {
            Ok(cfg) => Ok(cfg),
            Err(e) => {
                let old_path = util::append_suffix(&path, "old");
                paths::copy(&path, &old_path)?;
                Config::save(&Config::default())?;

                eprintln!("Incompatible config file found, replaced with default config");
                eprintln!("Old config file can be found at: {}", old_path.display());
                debug!("error: {}", e);

                Config::load()
            }
        }
    }

    /// Save the configuration.
    pub fn save(config: &Config) -> CIResult<()> {
        let mut path = util::config_path()?;
        path.push("default.cfg");
        let s = toml::to_string_pretty(config)?;
        paths::write(path, s)
    }
}
