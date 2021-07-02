use thiserror::Error;

#[derive(Error, Debug)]
pub enum CIError {
    #[error(
        "Compiler Interrupts library is not installed\n\
        Run `cargo lib-ci --install` to install the library"
    )]
    LibraryNotInstalled,

    #[error(
        "Compiler Interrupts library is already installed\n\
        Path to the library: {0}"
    )]
    LibraryAlreadyInstalled(String),

    #[error(
        "LLVM version from Rust toolchain ({0}) does not match with \
        LLVM version from LLVM toolchain ({1})"
    )]
    LLVMVersionNotMatch(String, String),

    #[error(
        "Unable to locate LLVM compiler toolchain\n\
        Check your $PATH variable or reinstall the toolchain"
    )]
    LLVMNotInstalled,

    #[error(
        "Failed to execute the binary '{0}'\n\
        Available binaries: {1}"
    )]
    BinaryNotAvailable(String, String),

    #[error("Package does not have any available binaries")]
    BinaryNotFound,

    #[error(
        "Could not determine which binary to run\n\
        Run `cargo run-ci --bin <BINARY_NAME>` to specify a binary\n\
        Available binaries: {0}"
    )]
    BinaryNotDetermine(String),

    #[error("Given path is not a valid directory: {0}")]
    PathNotDirectory(String),
}
