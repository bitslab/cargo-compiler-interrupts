use cargo_ci::{args, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    color_eyre::config::HookBuilder::default()
        .panic_section("Please report the bug at https://github.com/bitslab/cargo-ci")
        .install()?;

    let dargs = util::drop_name_args(cargo_ci::LIB_CI);

    let args = args::LibraryArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_dir_root()?;

    ops::library::exec(args)?;

    Ok(())
}
