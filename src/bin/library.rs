use cargo_compiler_interrupts::{args, config, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    let config = config::Config::load()?;

    let dargs = std::env::args().skip(1).collect::<Vec<_>>();

    let args = args::LibraryArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_package_root_dir()?;

    ops::library::exec(config, args)?;

    Ok(())
}
