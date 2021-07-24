use cargo_compiler_interrupts::{config, ops, opts, util, CIResult};
use clap::Clap;

/// Entry function of `cargo lib-ci`.
fn main() -> CIResult<()> {
    let config = config::Config::load()?;

    let args = std::env::args().skip(1).collect::<Vec<_>>();

    let library_opts = opts::LibraryOpts::parse_from(args);

    util::init_logger(library_opts.verbose);

    ops::library::exec(config, library_opts)?;

    Ok(())
}
