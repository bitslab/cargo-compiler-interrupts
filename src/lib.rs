pub type CIResult<T> = color_eyre::eyre::Result<T>;

pub mod args;
pub mod cargo;
pub mod config;
pub mod ops;
pub mod util;

pub static BUILD_CI: &str = "build-ci";
pub static RUN_CI: &str = "run-ci";
pub static LIB_CI: &str = "lib-ci";
