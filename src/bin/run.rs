/// Entry function of `cargo-run-ci`.
fn main() -> anyhow::Result<()> {
    cargo_compiler_interrupts::ops::run::exec()
}
