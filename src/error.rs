//! Errors related to the Compiler Interrupts integration.

// thiserror's bug
#![allow(clippy::nonstandard_macro_braces)]

use thiserror::Error;

/// Error types.
#[derive(Error, Debug)]
pub enum CIError {
    /// Compiler Interrupts library is not installed.
    #[error(
        "Compiler Interrupts library is not installed\n\
        Run `cargo lib-ci --install` to install the library"
    )]
    LibraryNotInstalled,

    /// Compiler Interrupts library is already installed.
    #[error(
        "Compiler Interrupts library is already installed\n\
        Path to the library: {0}\n\
        Run `cargo lib-ci --update` to update the library if needed"
    )]
    LibraryAlreadyInstalled(
        /// Path to the library.
        String,
    ),

    /// LLVM version between Rust and LLVM toolchain does not match.
    #[error(
        "LLVM version from Rust toolchain ({0}) does not match with the\
        LLVM version from LLVM toolchain ({1})"
    )]
    LLVMVersionNotMatch(
        /// LLVM version from Rust toolchain.
        String,
        /// LLVM version from LLVM toolchain.
        String,
    ),

    /// LLVM toolchain is not installed.
    #[error(
        "Unable to locate the LLVM compiler toolchain\n\
        Check your $PATH variable or reinstall the toolchain"
    )]
    LLVMNotInstalled,

    /// LLVM version is not supported.
    #[error("LLVM version {0} is not supported. Minimum supported LLVM version is {1}")]
    LLVMNotSupported(
        /// LLVM version.
        String,
        /// Minimum supported LLVM version
        String,
    ),

    /// Binary is not available.
    #[error(
        "Failed to execute the binary '{0}'\n\
        Available binaries: {1}"
    )]
    BinaryNotAvailable(
        /// Name of the unavailable binary.
        String,
        /// List of available binaries.
        String,
    ),

    /// Package does not have any available binaries.
    #[error("Package does not have any available binaries")]
    BinaryNotFound,

    /// Failed to determine which binary to run.
    #[error(
        "Could not determine which binary to run\n\
        Run `cargo run-ci --bin <BINARY_NAME>` to specify a binary\n\
        Available binaries: {0}"
    )]
    BinaryNotDetermine(
        /// List of available binaries.
        String,
    ),

    /// Path is not a valid directory.
    #[error("Given path is not a valid directory: {0}")]
    PathNotDirectory(
        /// Given path.
        String,
    ),
}
