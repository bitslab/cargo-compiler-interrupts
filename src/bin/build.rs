use cargo_compiler_interrupts::{config, ops, opts, util, CIResult};
use clap::Clap;

/// Entry function of `cargo build-ci`.
fn main() -> CIResult<()> {
    let config = config::Config::load()?;

    let args = std::env::args().skip(1).collect::<Vec<_>>();

    let build_opts = opts::BuildOpts::parse_from(args);

    util::init_logger(build_opts.verbose);

    util::set_current_package_root_dir()?;

    ops::build::exec(config, build_opts)?;

    Ok(())
}
