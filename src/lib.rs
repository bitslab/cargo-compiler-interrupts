pub type CIResult<T> = anyhow::Result<T>;

pub mod args;
pub mod cargo;
pub mod config;
pub mod error;
pub mod ops;
pub mod util;
