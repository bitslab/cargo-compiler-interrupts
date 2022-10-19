//! Errors related to the Compiler Interrupts integration.

use semver::Version;
use thiserror::Error;

/// Error types.
#[allow(dead_code)]
#[derive(Debug, Error)]
pub enum Error {
    /// Compiler Interrupts library is not installed.
    #[error(
        "Compiler Interrupts library is not installed\n\
        Run `cargo-lib-ci install` to install the library"
    )]
    LibraryNotInstalled,

    /// Compiler Interrupts library is already installed.
    #[error("Compiler Interrupts library is already installed")]
    LibraryAlreadyInstalled,

    /// LLVM version between Rust and LLVM toolchain does not match.
    #[error(
        "LLVM version from Rust toolchain ({0}) does not match with \
        the version from LLVM toolchain ({1})"
    )]
    LLVMVersionNotMatch(Version, Version),

    /// LLVM toolchain is not installed.
    #[error("Unable to locate the LLVM compiler toolchain")]
    LLVMNotInstalled,

    /// LLVM version is not supported.
    #[error(
        "LLVM version {0} is not supported. Supported LLVM versions are from {} to before {}",
        crate::llvm::LLVM_MIN_VERSION,
        crate::llvm::LLVM_MAX_VERSION
    )]
    LLVMNotSupported(Version),

    /// Binary is not available.
    #[error(
        "Failed to execute the binary '{0}'\n\
        Available binaries: {1}"
    )]
    BinaryNotAvailable(String, String),

    /// Failed to determine which binary to run.
    #[error(
        "Could not determine which binary to run\n\
        Run `cargo-run-ci --bin <NAME>` to specify a binary\n\
        Available binaries: {0}"
    )]
    BinaryNotDetermine(String),

    /// Package does not have any available CI-integrated binaries.
    #[error(
        "Package does not have any available CI-integrated binaries\n\
        Run `cargo-build-ci` to build the package first"
    )]
    IntegratedBinaryNotFound,

    /// Package does not have any available binaries.
    #[error("Package does not have any available binaries")]
    BinaryNotFound,
}
