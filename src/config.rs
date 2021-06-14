use cargo_util::paths;
use serde::{Deserialize, Serialize};
use tracing::{debug, info};

use crate::{util, CIResult};

#[derive(Serialize, Deserialize, Default)]
pub struct Config {
    pub library_path: String,
    pub llvm_version: String,
    pub default_args: Vec<String>,
}

impl Config {
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

                eprintln!("broken config file found, replaced with default config");
                eprintln!("old config file can be found at: {}\n", old_path.display());
                debug!("error: {}", e);

                Config::load()
            }
        }
    }

    pub fn save(config: &Config) -> CIResult<()> {
        let mut path = util::config_path()?;
        path.push("default.cfg");
        let s = toml::to_string_pretty(config)?;
        paths::write(path, s)
    }
}
