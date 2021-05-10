use clap::{ArgGroup, Clap};

#[derive(Clap, Debug)]
#[clap(
    name = format!("cargo-{}", crate::BUILD_CI),
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Compile and integrate the Compiler Interrupts to a local package",
)]
pub struct BuildArgs {
    /// Build artifacts in release mode
    #[clap(short, long)]
    pub release: bool,

    /// Build for the target triple
    #[clap(short, long, value_name = "TRIPLE")]
    pub target: Option<String>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}

#[derive(Clap, Debug)]
#[clap(
    name = format!("cargo-{}", crate::RUN_CI),
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Run a Compiler Interrupts-integrated binary",
)]
pub struct RunArgs {
    /// Run the binary in release mode
    #[clap(short, long)]
    pub release: bool,

    /// Run for the target triple
    #[clap(short, long, value_name = "TRIPLE")]
    pub target: Option<String>,

    /// Name of the binary
    #[clap(short, long)]
    pub bin: Option<String>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}

#[derive(Clap, Debug)]
#[clap(
    name = format!("cargo-{}", crate::LIB_CI),
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = "Manage the Compiler Interrupts library",
    group = ArgGroup::new("lib").required(true),
)]
pub struct LibraryArgs {
    /// Install the library
    #[clap(short, long, group = "lib")]
    pub install: bool,

    /// Uninstall the library
    #[clap(short, long, group = "lib")]
    pub uninstall: bool,

    /// Set default arguments for the library
    #[clap(short, long, requires = "lib", default_values = &[
        "-clock-type=1",
        "-config=2",
        "-inst-gran=2",
        "-all-dev=100",
        "-push-intv=5000",
        "-commit-intv=1000",
        "-mem-ops-cost=1",
        "-fiber-config=5"
    ])]
    pub args: Vec<String>,

    /// Path to the library when installing
    #[clap(short, long, requires = "lib")]
    pub path: Option<String>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}
