use cargo_ci::{args, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    color_eyre::config::HookBuilder::default()
        .panic_section("Consider reporting the bug at https://github.com/bitslab/cargo-ci")
        .install()?;

    let dargs = util::drop_name_args(cargo_ci::RUN_CI);

    let args = args::RunArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_dir_root()?;

    ops::run::exec(args)?;

    Ok(())
}
