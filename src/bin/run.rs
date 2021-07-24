use cargo_compiler_interrupts::{ops, opts, util, CIResult};
use clap::Clap;

/// Entry function of `cargo run-ci`.
fn main() -> CIResult<()> {
    let args = std::env::args().skip(1).collect::<Vec<_>>();

    let run_opts = opts::RunOpts::parse_from(args);

    util::init_logger(run_opts.verbose);

    util::set_current_package_root_dir()?;

    ops::run::exec(run_opts)?;

    Ok(())
}
