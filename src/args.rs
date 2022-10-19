//! Handles arguments for the subcommands.

use clap::builder::PossibleValuesParser;
use clap::{Args, Parser, Subcommand};

use crate::*;

/// Compile and integrate the Compiler Interrupts to a package
#[derive(Debug, Parser)]
#[command(name = BUILD_CI_BIN_NAME, author, version)]
pub struct BuildArgs {
    /// Crates to skip the integration (space-delimited)
    #[arg(long = "skip", value_delimiter = ' ', value_name = "CRATES")]
    pub skip_crates: Option<Vec<String>>,

    /// Enable debugging mode for Compiler Interrupts library
    #[arg(long)]
    pub debug: bool,

    /// Arguments for `cargo` invocation
    #[arg(value_name = "CARGO_BUILD_ARGS", raw = true)]
    pub cargo_args: Vec<String>,

    /// Log level
    #[arg(
        long = "log",
        default_value = "warn",
        value_parser = PossibleValuesParser::new(["trace", "debug", "info", "warn", "error"]),
        value_name = "LEVEL",
        global = true,
    )]
    pub log_level: String,
}

/// Run a Compiler Interrupts-integrated binary
#[derive(Debug, Parser)]
#[command(name = RUN_CI_BIN_NAME, author, version, trailing_var_arg = true)]
pub struct RunArgs {
    /// Name of the binary
    #[arg(long = "bin", value_name = "NAME")]
    pub binary_name: Option<String>,

    /// Arguments for the binary
    #[arg(raw = true, value_name = "ARGS")]
    pub binary_args: Vec<String>,

    /// Arguments for `cargo` invocation
    #[arg(value_name = "CARGO_RUN_ARGS", raw = true)]
    pub cargo_args: Vec<String>,

    /// Log level
    #[arg(
        long = "log",
        default_value = "warn",
        value_parser = PossibleValuesParser::new(["trace", "debug", "info", "warn", "error"]),
        value_name = "LEVEL",
        global = true,
    )]
    pub log_level: String,
}

/// Manage the Compiler Interrupts library
#[derive(Debug, Parser)]
#[command(name = LIB_CI_BIN_NAME, author, version)]
pub struct LibraryArgs {
    /// Subcommands for managing the library
    #[command(subcommand)]
    pub command: Option<LibrarySubcommands>,

    /// Log level
    #[arg(
        long = "log",
        default_value = "warn",
        value_parser = PossibleValuesParser::new(["trace", "debug", "info", "warn", "error"]),
        value_name = "LEVEL",
        global = true,
    )]
    pub log_level: String,
}

/// Subcommands for managing the library
#[derive(Debug, Subcommand)]
pub enum LibrarySubcommands {
    /// Install the Compiler Interrupts library
    Install(InstallArgs),

    /// Uninstall the Compiler Interrupts library
    Uninstall,

    /// Update the Compiler Interrupts library
    Update,

    /// Configure the Compiler Interrupts library
    Config(ConfigArgs),
}

/// Arguments for installing the library
#[derive(Args, Debug)]
pub struct InstallArgs {
    /// Install path for the library
    #[arg(long, value_name = "PATH")]
    pub path: Option<String>,

    /// URL to the source code of the library. Use `file://` for local files.
    #[arg(long, value_name = "URL")]
    pub url: Option<String>,
}

/// Arguments for configuring the library
#[derive(Args, Debug)]
pub struct ConfigArgs {
    /// Default arguments for the library (space-delimited)
    #[arg(
        long,
        allow_hyphen_values = true,
        use_value_delimiter = true,
        value_delimiter = ' ',
        value_name = "ARGS"
    )]
    pub library_args: Option<Vec<String>>,
}
