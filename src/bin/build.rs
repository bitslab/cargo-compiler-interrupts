/// Entry function of `cargo-build-ci`.
fn main() -> anyhow::Result<()> {
    cargo_compiler_interrupts::ops::build::exec()
}
