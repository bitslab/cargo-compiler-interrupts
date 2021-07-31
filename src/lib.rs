//! [![crates.io](https://img.shields.io/crates/v/cargo-compiler-interrupts.svg)][crates.io]
//! [![docs.rs](https://docs.rs/cargo-compiler-interrupts/badge.svg)][docs.rs]
//! [![license](https://img.shields.io/crates/l/cargo-compiler-interrupts.svg)][license]
//!
//! `cargo-compiler-interrupts` provides you a seamless way to integrate the
//! [Compiler Interrupts][compiler-interrupts-paper] to any Rust packages.
//! Check out the Compiler Interrupts [main repository][compiler-interrupts] for more info.
//!
//! ## Requirements
//!
//! * [Rust 1.45.0][rust] or later and [LLVM 9][llvm] or later are required.
//! Both must have the same LLVM version.
//! * You can check the LLVM version from Rust toolchain and LLVM toolchain by running `rustc -vV`
//! and `llvm-config --version` respectively.
//! * x86-64 architecture with Linux or macOS is highly recommended.
//! Other architectures and platforms have not been tested.
//!
//! ## Installation
//!
//! `cargo-compiler-interrupts` can be installed via `cargo install`.
//!
//! ``` sh
//! cargo install cargo-compiler-interrupts
//! ```
//!
//! You can also fetch the repo and install  using `--path`.
//!
//! ``` sh
//! git clone https://github.com/bitslab/cargo-compiler-interrupts
//! cargo install --path ./cargo-compiler-interrupts
//! ```
//!
//! ## Getting started
//!
//! `cargo-compiler-interrupts` provides three subcommands:
//!
//! ``` sh
//! cargo lib-ci --install    # install the CI library
//! cargo build-ci            # build and integrate CI to the binary
//! cargo run-ci              # run the CI-integrated binary
//! ```
//!
//! * `cargo lib-ci` — manage the Compiler Interrupts library.
//! * `cargo build-ci` — build and integrate the Compiler Interrupts to the package.
//! * `cargo run-ci` — run the integrated binary.
//! You can specify which binary to run by passing `--bin <BINARY>`.
//!
//! Run `cargo lib-ci --install` to install the Compiler Interrupts library first.
//! Before running `cargo build-ci`, add the Compiler Interrupts API package as the dependency for
//! your Cargo package and registers the Compiler Interrupts handler in your program.
//! Compiler Interrupts API is provided through the [`compiler-interrupts`][compiler-interrupts-rs]
//! package.
//!
//! ``` rust
//! fn interrupt_handler(ic: i64) {
//!     println!("Compiler interrupt called with instruction count: {}", ic);
//! }
//!
//! unsafe {
//!     compiler_interrupts::register(1000, 1000, interrupt_handler);
//! }
//! ```
//!
//! For more detailed usages and internals, run the command with `--help` option and
//! check out the **[documentation]**.
//!
//! ## Contribution
//!
//! All issue reports, feature requests, pull requests and GitHub stars are welcomed
//! and much appreciated. Issues relating to the Compiler Interrupts library
//! should be reported to the [main repository][compiler-interrupts].
//!
//! ## Author
//!
//! Quan Tran ([@quanshousio][quanshousio])
//!
//! ## Acknowledgements
//!
//! * My advisor [Jakob Eriksson][jakob] for the enormous support for this project.
//! * [Nilanjana Basu][nilanjana] for implementing the Compiler Interrupts.
//!
//! ## License
//!
//! `cargo-compiler-interrupts` is available under the MIT license.
//! See the [LICENSE][license] file for more info.
//!
//! [crates.io]: https://crates.io/crates/cargo-compiler-interrupts
//! [docs.rs]: https://docs.rs/cargo-compiler-interrupts
//! [license]: https://github.com/bitslab/cargo-compiler-interrupts/blob/main/LICENSE
//! [documentation]: https://github.com/bitslab/cargo-compiler-interrupts/blob/main/DOCUMENTATION.md
//! [compiler-interrupts]: https://github.com/bitslab/CompilerInterrupts
//! [compiler-interrupts-rs]: https://github.com/bitslab/compiler-interrupts-rs
//! [compiler-interrupts-paper]: https://dl.acm.org/doi/10.1145/3453483.3454107
//! [rust]: https://www.rust-lang.org/tools/install
//! [llvm]: https://releases.llvm.org
//! [quanshousio]: https://quanshousio.com
//! [jakob]: https://www.linkedin.com/in/erikssonjakob
//! [nilanjana]: https://www.linkedin.com/in/nilanjana-basu-99027959

#![warn(
    missing_copy_implementations,
    missing_debug_implementations,
    missing_docs,
    trivial_casts,
    trivial_numeric_casts,
    unsafe_code,
    unstable_features,
    unused_extern_crates,
    unused_import_braces,
    unused_qualifications,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::clone_on_ref_ptr,
    clippy::missing_docs_in_private_items,
    clippy::mut_mut,
    clippy::print_stdout,
    clippy::unseparated_literal_suffix,
    clippy::unwrap_used
)]

/// Compiler Interrupts result.
pub type CIResult<T> = anyhow::Result<T>;

pub mod config;
pub mod error;
pub mod ops;
pub mod opts;
pub mod util;
