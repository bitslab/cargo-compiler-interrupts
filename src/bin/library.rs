/// Entry function of `cargo-lib-ci`.
fn main() -> anyhow::Result<()> {
    cargo_compiler_interrupts::ops::library::exec()
}
