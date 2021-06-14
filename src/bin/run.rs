use cargo_ci::{args, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    let dargs = util::drop_name_args(cargo_ci::RUN_CI);

    let args = args::RunArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_package_root_dir()?;

    ops::run::exec(args)?;

    Ok(())
}
