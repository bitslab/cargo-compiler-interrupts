# cargo-compiler-interrupts

[![crates.io](https://img.shields.io/crates/v/cargo-compiler-interrupts.svg)](https://crates.io/crates/cargo-compiler-interrupts)
[![docs.rs](https://docs.rs/cargo-compiler-interrupts/badge.svg)](https://docs.rs/cargo-compiler-interrupts)
[![license](https://img.shields.io/crates/l/cargo-compiler-interrupts.svg)](LICENSE)

`cargo-compiler-interrupts` provides you a seamless way to integrate the [Compiler Interrupts](https://dl.acm.org/doi/10.1145/3453483.3454107) to any Rust packages. Check out the Compiler Interrupts [main repository](https://github.com/bitslab/CompilerInterrupts) for more info.

## Requirements

* [Rust 1.45.0](https://www.rust-lang.org/tools/install) or later and [LLVM 9](https://releases.llvm.org/) or later are required. Both must have the same LLVM version.
* You can check the LLVM version from Rust toolchain and LLVM toolchain by running `rustc -vV` and `llvm-config --version` respectively.
* x86-64 architecture with Linux or macOS is highly recommended. Other architectures and platforms have not been tested.

## Installation

`cargo-compiler-interrupts` is a Cargo package and can be installed via `cargo install`.

``` sh
cargo install cargo-compiler-interrupts
```

You can also fetch the repo and install  using `--path`.

``` sh
git clone https://github.com/bitslab/cargo-compiler-interrupts
cargo install --path ./cargo-compiler-interrupts
```

## Getting started

`cargo-compiler-interrupts` provides three subcommands:

``` sh
cargo lib-ci --install    # install the CI library
cargo build-ci            # build and integrate CI to the binary
cargo run-ci              # run the CI-integrated binary
```

* `cargo lib-ci` — manage the Compiler Interrupts library.
* `cargo build-ci` — build and integrate the Compiler Interrupts to the package.
* `cargo run-ci` — run the integrated binary. You can specify which binary to run by passing `--bin <BINARY>`.

Make sure your program registers the Compiler Interrupts handler before running `cargo build-ci`. Compiler Interrupts APIs are provided through the [`compiler-interrupts`](https://github.com/bitslab/compiler-interrupts-rs) package.

``` rust
fn interrupt_handler(ic: i64) {
    println!("Compiler interrupt called with instruction count: {}", ic);
}

unsafe {
    compiler_interrupts::register(1000, 1000, interrupt_handler);
}
```

For more detailed usages and internals, run the command with `--help` option and check out the **[documentation](DOCUMENTATION.md)**.

## Contribution

All issue reports, feature requests, pull requests and GitHub stars are welcomed and much appreciated. Issues relating to the Compiler Interrupts integration should be reported to the [main repository](https://github.com/bitslab/CompilerInterrupts).

## Author

Quan Tran ([@quanshousio](https://quanshousio.com))

## Acknowledgements

* My advisor [Jakob Eriksson](https://www.linkedin.com/in/erikssonjakob) for the enormous support for this project.
* [Nilanjana Basu](https://www.linkedin.com/in/nilanjana-basu-99027959) for implementing the Compiler Interrupts.

## License

`cargo-compiler-interrupts` is available under the MIT license. See the [LICENSE](LICENSE) file for more info.
