use clap::Clap;
use std::sync::{RwLock, RwLockReadGuard};

lazy_static::lazy_static! {
    static ref ARGS: RwLock<Args> = RwLock::new(Args::parse());
}

#[derive(Clap, Debug)]
#[clap(
    name = clap::crate_name!(),
    version = clap::crate_version!(),
    author = clap::crate_authors!(),
    about = clap::crate_description!(),
)]
pub struct Args {
    /// Build artifacts mode
    #[clap(
        long,
        value_name = "MODE",
        default_value = "debug",
        possible_values = &["debug", "release"],
        hide_default_value = true
    )]
    pub build_mode: String,

    /// Build for the target triple
    #[clap(long, value_name = "TRIPLE")]
    pub target: Option<String>,

    /// Specify the path for the Compiler Interrupt library
    #[clap(long)]
    pub lib_path: Option<String>,

    /// Specify the arguments for the pre-CI integration `opt` instance
    #[clap(long)]
    pub pre_ci_args: Vec<String>,

    /// Specify the arguments for the CI integration `opt` instance
    #[clap(long)]
    pub ci_args: Vec<String>,

    /// Use verbose output (-vv very verbose output)
    #[clap(short, long, parse(from_occurrences))]
    pub verbose: i32,
}

pub(crate) fn get() -> RwLockReadGuard<'static, Args> {
    ARGS.read().unwrap()
}
