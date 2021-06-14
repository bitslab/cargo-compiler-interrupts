# Change Log

All notable changes to this project will be documented in this file. `cargo-ci` adheres to [Semantic Versioning](https://semver.org/).

#### 2.x Releases

- `2.0.x` Releases - [2.0.0](#200) | [2.1.0](#210)

#### 1.x Releases

- `1.0.x` Releases - [1.0.0](#100)

---

## [2.1.0](https://github.com/bitslab/cargo-ci/releases/tag/2.1.0)

Released on 2021-06-14.

- Initial release of `cargo-ci`.

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

## [2.0.0](https://github.com/bitslab/cargo-ci/releases/tag/2.0.0)

Released on 2021-05-10.

#### Added

- Installing, uninstalling and display information about the CI library through `cargo-lib-ci`.
- Running CI-integrated binaries through `cargo-run-ci`.
- External configuration file of the CI library in `CONFIG_DIR`, which is platform dependent.

#### Updated

- Required minimum version for dependencies has been narrowed.
- Path to the library file is moved to `CONFIG_DIR`.
- Running `cargo-ci` utilities through `cargo` subcommand.
- Running `cargo-ci` utilities when current directory is not at the root directory of the package.

#### Removed

- `build.rs` has been removed. Please use `cargo-lib-ci` to handle the library's installation.
- `libci` has been removed. Source code for CI is available [here](https://github.com/bitslab/CompilerInterrupts). Source code will be feteched from this repository when needed.
- Skipped crates while integrating the CI have been removed.

---

## [1.0.0](https://github.com/bitslab/cargo-ci/releases/tag/1.0.0)

Released on 2021-05-05.

#### Added

- Initial release of `cargo-ci`.
