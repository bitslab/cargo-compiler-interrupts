# cargo-compiler-interrupts

`cargo-compiler-interrupts` provides you a simple way to integrate the [Compiler Interrupts](https://pldi21.sigplan.org/details/pldi-2021-papers/82/Frequent-Background-Polling-on-a-Shared-Thread-using-Light-Weight-Compiler-Interrupt) to any Cargo packages.

## Requirements

* [Rust 1.52.0](https://www.rust-lang.org/tools/install) or later and [LLVM 12.0.0](https://releases.llvm.org/) or later are required. Both must have the same LLVM version.
* You can check the LLVM version from Rust toolchain and LLVM toolchain by running `rustc -vV` and `llvm-config --version` respectively.
* x86-64 architecture with Linux or macOS is highly recommended. Other architectures and platforms have not been tested.

## Installation

`cargo-compiler-interrupts` is a Cargo package and can be installed via `cargo install` .

``` sh
cargo install cargo-compiler-interrupts
```

You can also fetch the repo and install using `--path`.

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

* `cargo lib-ci` — install or uninstall the Compiler Interrupts library.
* `cargo build-ci` — build and integrate the Compiler Interrupts to the binary.
* `cargo run-ci` — run the integrated binary. You can specify which binary to run by passing `--bin <BINARY_NAME>`.

For more detailed usage and internals, please run the command with `--help` option and check out the **[documentation](DOCUMENTATION.md)**.

## Contribution

All issue reports, feature requests, pull requests and GitHub stars are welcomed and much appreciated.

## Author

Quan Tran ([@quanshousio](https://quanshousio.com))

## Acknowledgements

* My advisor [Jakob Eriksson](https://www.linkedin.com/in/erikssonjakob) for the enormous support for this project.
* [Nilanjana Basu](https://www.linkedin.com/in/nilanjana-basu-99027959) for implementing the Compiler Interrupts.

## License

`cargo-compiler-interrupts` is available under the MIT license. See the [LICENSE](LICENSE) file for more info.
