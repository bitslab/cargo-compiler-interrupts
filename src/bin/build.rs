use cargo_compiler_interrupts::{args, config, ops, util, CIResult};
use clap::Clap;

pub fn main() -> CIResult<()> {
    let config = config::Config::load()?;

    let dargs = std::env::args().skip(1).collect::<Vec<_>>();

    let args = args::BuildArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_package_root_dir()?;

    ops::build::exec(config, args)?;

    Ok(())
}
