# cargo-ci

`cargo-ci` provides you a simple way to integrate the [Compiler Interrupts](https://pldi21.sigplan.org/details/pldi-2021-papers/82/Frequent-Background-Polling-on-a-Shared-Thread-using-Light-Weight-Compiler-Interrupt) to any Cargo packages.

## Requirements

* [Rust 1.52.0](https://www.rust-lang.org/tools/install) or later and [LLVM 12.0.0](https://releases.llvm.org/) or later are required. Both must have the same LLVM version.
* You can check the LLVM version from Rust toolchain and LLVM toolchain by running `rustc -vV` and `llvm-config --version` respectively.
* x86-64 architecture with Linux or macOS is highly recommended. Other architectures and platforms have not been tested.

## Installation

`cargo-ci` is a Cargo subcommand and can be installed via `cargo install` .

``` sh
cargo install cargo-ci
```

## Getting started

``` sh
cargo lib-ci --install    # install the CI library
cargo build-ci            # build and integrate CI to the binary
cargo run-ci              # run the CI-integrated binary
```

* `cargo lib-ci` to install or uninstall the Compiler Interrupts library.
* `cargo build-ci` to build and integrate the Compiler Interrupts to the binary.
* `cargo run-ci` to run the integrated binary. You can specify which binary to run by passing `--bin <binary_name>`.

For more detailed usage and internals, please run the command with `--help` option and check out the **[documentation](DOCUMENTATION.md)**.

## Contribution

All issue reports, feature requests, pull requests and GitHub stars are welcomed and much appreciated.

## Author

Quan Tran ([@quanshousio](https://quanshousio.com))

## Acknowledgements

* My advisor [Jakob Eriksson](https://www.linkedin.com/in/erikssonjakob) for the enormous support for this project.
* [Nilanjana Basu](https://www.linkedin.com/in/nilanjana-basu-99027959) for implementing the Compiler Interrupts.

## License

`cargo-ci` is available under the MIT license. See the [LICENSE](LICENSE) file for more info.
