use cargo_ci::{args, cargo, ops, util, CIResult};
use clap::Clap;

fn main() -> CIResult<()> {
    color_eyre::config::HookBuilder::default()
        .panic_section("Consider reporting the bug at https://github.com/bitslab/cargo-ci")
        .install()?;

    let dargs = util::drop_name_args(cargo_ci::BUILD_CI);

    let args = args::BuildArgs::parse_from(dargs);

    util::init_logger(args.verbose);

    util::set_current_dir_root()?;

    if let Err(e) = ops::build::exec(args) {
        cargo::clean()?;
        return Err(e);
    };

    Ok(())
}
