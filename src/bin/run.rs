use cargo_compiler_interrupts::{args, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    let dargs = std::env::args().skip(1).collect::<Vec<_>>();

    let args = args::RunArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_package_root_dir()?;

    ops::run::exec(args)?;

    Ok(())
}
