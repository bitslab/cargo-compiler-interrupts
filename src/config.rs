use std::fs;

use serde::{Deserialize, Serialize};

use crate::{util, CIResult};

#[derive(Clone, Serialize, Deserialize)]
pub struct Config {
    pub path: String,
    pub llvm_version: String,
    pub args: Vec<String>,
}

impl Config {
    pub fn load() -> CIResult<Config> {
        let mut path = util::config_path()?;
        path.push("default.cfg");
        let s = fs::read_to_string(path)?;
        let cfg = toml::from_str(&s)?;
        Ok(cfg)
    }

    pub fn save(&self) -> CIResult<()> {
        let mut path = util::config_path()?;
        path.push("default.cfg");
        let s = toml::to_string_pretty(self)?;
        fs::write(path, s)?;
        Ok(())
    }
}
