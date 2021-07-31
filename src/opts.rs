//! Handles options for the subcommands.

use clap::Clap;

/// Options for `cargo-build-ci`.
#[derive(Clap, Debug)]
#[clap(
    name = "cargo-build-ci",
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Compile and integrate the Compiler Interrupts to a package",
)]
pub struct BuildOpts {
    /// Build artifacts in release mode
    #[clap(short, long)]
    pub release: bool,

    /// Build for the target triple
    #[clap(short, long, value_name = "TRIPLE")]
    pub target: Option<String>,

    /// Build an example artifact
    #[clap(short, long, value_name = "BINARY")]
    pub example: Option<String>,

    /// Crates to skip the integration (space-delimited)
    #[clap(
        short,
        long,
        value_name = "CRATES",
        require_delimiter = true,
        value_delimiter = " "
    )]
    pub skip_crates: Option<Vec<String>>,

    /// Enable debugging mode for the library when integrating
    #[clap(short, long)]
    pub debug_ci: bool,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}

/// Options for `cargo-run-ci`.
#[derive(Clap, Debug)]
#[clap(
    name = "cargo-run-ci",
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Run a Compiler Interrupts-integrated binary",
    setting = clap::AppSettings::TrailingVarArg,
)]
pub struct RunOpts {
    /// Run the binary in release mode
    #[clap(short, long)]
    pub release: bool,

    /// Target triple for the binary
    #[clap(short, long, value_name = "TRIPLE")]
    pub target: Option<String>,

    /// Name of the binary
    #[clap(short, long, value_name = "BINARY")]
    pub bin: Option<String>,

    /// Arguments for the binary
    #[clap(short, long, value_name = "ARGS")]
    pub args: Option<Vec<String>>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}

/// Options for `cargo-lib-ci`.
#[derive(Clap, Debug)]
#[clap(
    name = "cargo-lib-ci",
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Manage the Compiler Interrupts library"
)]
pub struct LibraryOpts {
    /// Install the library
    #[clap(short, long, conflicts_with_all = &["uninstall", "update"])]
    pub install: bool,

    /// Uninstall the library
    #[clap(short, long, conflicts_with_all = &["install", "update"])]
    pub uninstall: bool,

    /// Update the library
    #[clap(long, conflicts_with_all = &["install", "uninstall"])]
    pub update: bool,

    /// Default arguments for the library (space-delimited)
    #[clap(
        short,
        long,
        allow_hyphen_values = true,
        require_delimiter = true,
        value_delimiter = " ",
        value_name = "ARGS"
    )]
    pub args: Option<Vec<String>>,

    /// Remote URL to the source code of the library when installing
    #[clap(long, value_name = "URL", requires = "install")]
    pub url: Option<String>,

    /// Destination path for the library when installing
    #[clap(short, long, value_name = "PATH", requires = "install")]
    pub path: Option<String>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}
