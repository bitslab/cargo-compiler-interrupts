use cargo_ci::{args, config, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    let config = config::Config::load()?;

    let dargs = util::drop_name_args(cargo_ci::LIB_CI);

    let args = args::LibraryArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_package_root_dir()?;

    ops::library::exec(config, args)?;

    Ok(())
}
