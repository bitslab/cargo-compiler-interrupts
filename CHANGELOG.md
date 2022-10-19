# Change Log

All notable changes to this project will be documented in this file. `cargo-compiler-interrupts` adheres to [Semantic Versioning](https://semver.org/).

#### 3.x Releases

- `4.0.x` Releases - [4.0.0](#400)
- `3.2.x` Releases - [3.2.0](#320) | [3.2.1](#321)
- `3.1.x` Releases - [3.1.0](#310)
- `3.0.x` Releases - [3.0.0](#300) | [3.0.1](#301)

#### 2.x Releases

- `2.1.x` Releases - [2.1.0](#210)
- `2.0.x` Releases - [2.0.0](#200)

#### 1.x Releases

- `1.0.x` Releases - [1.0.0](#100)

---

## [4.0.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/4.0.0)

Released on 2022-10-18.

#### Added

- Add `Cargo.lock` to lock dependency version.
- Major code refactoring:
  - New subcommands and options. Please check `--help` for further usage.
  - New `cargo` module contains `cargo`-related functions.
  - New `paths` module contains functions for manipulating `Path` and `PathBuf`.
  - New `llvm` module is reimplemented from `util:toolchain`.
  - New `args` module updated with clap 4.0, replaced the old `opts` module.
  - Better logging and error messages.

#### Updated

- Use Rust 2021.
- Major dependency updates such as clap 4.0, indicatif 0.17, and other dependencies.

#### Removed

- Local `cargo-util` package is replaced by using the one from upstream.

---

## [3.2.1](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/3.2.1)

Released on 2021-08-04.

#### Updated

- Documentation and dependencies.
- `cargo-build-ci`
  - Progress indicator is more aware of terminal size changes.

## [3.2.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/3.2.0)

Released on 2021-07-31.

#### Added

- `cargo-build-ci`
  - Support integration for `examples` target by passing `--example` option.
- `cargo-run-ci`
  - Ability to pass arguments to the binary by passing `--args` option.

#### Updated

- Various path-related helper functions are renamed.
- `cargo-build-ci`
  - Fix a race condition between integration threads and progress indicator thread when integration thread failed unexpectedly.
  - Failed integration can now be run again without using `cargo clean`.
- `cargo-lib-ci`
  - Fix wrong output formatting while the progress indicator is showing.

#### Removed

- `deps-ci` folder has been removed. All CI-integrated artifacts are now resided in the same directory as the original one.

## [3.1.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/3.1.0)

Released on 2021-07-24.

#### Added

- Compiler Interrupts library officially supports LLVM from 9 to 12.
- Documentation for all public items.
- Configuration has three new fields: Path to the debug-enabled library, checksum of the source and remote URL for the source code.
- `cargo-build-ci`
  - Ability to skip crates while integration by passing `--skip-crates` option.
  - Ability to enable debugging mode when integrating by passing `--debug-ci` option.
  - Real-time output from internal `cargo build` invocation.
  - Progress indicator when running the integration.
  - Linker invocations now run concurrently.
  - Output from internal commands such as `opt` and linker is now truncated by default to prevent polluting the terminal. Passing `--debug-ci` enables full and verbose logging.
  - When running with `--debug-ci` option, output from failed process is written to a log file instead of directly printing to the terminal. Log files can be found in the same directory with the configuration file.
- `cargo-lib-ci`
  - Ability to update the library by passing `--update` option.
  - Ability to specify a remote URL to the source code of the library by passing `--url` option.
  - Installing the library now installs both with and without debugging mode versions.
  - File name for the library now has a checksum appended to it for versioning.
  - Progress indicator when installing/updating the library.

#### Updated

- `args` module is renamed to `opts`, same for `Args` structs.
- Using an unsupported version of LLVM will now throw an appropriate error.
- Various helper functions are renamed.
- `cargo-build-ci`
  - Fix the mismatch relocation symbols error when linking on Linux by passing `--code-model=large` to `llc` instead of using `relocation-model=static`.
- `cargo-lib-ci`
  - Fix an ambiguous use when user passes both `--install` and `--uninstall`.
  - Remove checking if the current directory is a valid Cargo directory.
  - Default arguments for the library are changed.

#### Removed

- `cargo` module has been removed. All Cargo related functions have been moved to `ops/build`.
- `wget` usage for fetching the source code has been removed.

## [3.0.1](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/3.0.1)

Released on 2021-07-02.

#### Updated

- Fix the compilation error when running `cargo publish`.
- Version number should now be correct.

## [3.0.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/3.0.0)

Released on 2021-07-02.

#### Updated

- `cargo-ci` has been renamed to `cargo-compiler-interrupts`.
- `cargo-build-ci`
  - Temporary fix to the mismatch relocation symbols error when linking on Linux by passing `relocation-model=static` to the compiler.

#### Removed

- Miscellaneous helper functions.

---

## [2.1.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/2.1.0)

Released on 2021-06-14.

#### Added

- `CIError` type for custom errors.
- Helper functions relating to manipulating `PathBuf`.

#### Updated

- Improve checking for incremental compilation.
- Improve error handling when loading the configuration.
- Improve finding the correct LLVM utilities based on LLVM version from `rustc`.
- Use `RUSTC_LOG` to output the internal linker invocation instead of `-Z print-link-args` which requires nightly Rust to work.

#### Removed

- Helper functions relating to running external LLVM utilities.

## [2.0.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/2.0.0)

Released on 2021-05-10.

#### Added

- Installing, uninstalling and display information about the Compiler Interrupts library through `cargo-lib-ci`.
- Running CI-integrated binaries through `cargo-run-ci`.
- External configuration file for the Compiler Interrupts library is now in `CONFIG_DIR`, which is platform dependent.

#### Updated

- Required minimum version for dependencies has been narrowed.
- Compiler Interrupts library file is moved to `CONFIG_DIR`.
- Ability to run the commands through `cargo`.
- Ability to run when current directory is not at the root directory of the package.

#### Removed

- `build.rs` has been removed. Please use `cargo-lib-ci` to handle the library's installation.
- `libci` has been removed. Source code for the Compiler Interrupts is available [here](https://github.com/bitslab/CompilerInterrupts). Source code will be fetched from this repository when needed.
- Default skipped crates while integrating the Compiler Interrupts have been removed.

---

## [1.0.0](https://github.com/bitslab/cargo-compiler-interrupts/releases/tag/1.0.0)

Released on 2021-05-05.

#### Added

- Initial release of `cargo-compiler-interrupts`.
