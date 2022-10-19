//! LLVM toolchain utilities.

use anyhow::{bail, Context};
use cargo_util::ProcessBuilder;
use semver::{Comparator, Op, Version};

use crate::error::Error;
use crate::CIResult;

/// Minimum LLVM version support.
pub const LLVM_MIN_VERSION: Version = Version::new(9, 0, 0);

/// Maximum LLVM version support.
pub const LLVM_MAX_VERSION: Version = Version::new(15, 0, 0);

/// LLVM utility.
#[derive(Copy, Clone, Debug)]
pub enum LlvmUtility {
    /// LLVM archiver.
    Archiver,
    /// LLVM config utility.
    Config,
    /// LLVM C language family front-end compiler.
    Clang,
    /// LLVM bitcode and symbols utility.
    NameMangling,
    /// LLVM optimizer.
    Optimizer,
    /// LLVM static compiler.
    StaticCompiler,
}

impl LlvmUtility {
    /// Gets the binary name of the utility.
    fn as_str(&self) -> &str {
        match self {
            LlvmUtility::Archiver => "llvm-ar",
            LlvmUtility::Config => "llvm-config",
            LlvmUtility::Clang => "clang",
            LlvmUtility::NameMangling => "llvm-nm",
            LlvmUtility::Optimizer => "opt",
            LlvmUtility::StaticCompiler => "llc",
        }
    }

    /// Gets the process builder given the toolchain information.
    pub fn process_builder(&self, toolchain: &LlvmToolchain) -> ProcessBuilder {
        if toolchain.suffix {
            ProcessBuilder::new(format!("{}-{}", self.as_str(), toolchain.version.major))
        } else {
            ProcessBuilder::new(self.as_str())
        }
    }
}

/// LLVM toolchain.
#[derive(Debug)]
pub struct LlvmToolchain {
    /// LLVM version.
    pub version: Version,

    /// LLVM utility contains version suffix.
    suffix: bool,
}

/// Get information about LLVM toolchain.
pub fn toolchain() -> CIResult<LlvmToolchain> {
    // get llvm version from rustc
    let output = ProcessBuilder::new("rustc").arg("-vV").exec_with_output()?;
    let stdout = String::from_utf8(output.stdout)?;
    let rustc_llvm_version = Version::parse(
        stdout
            .lines()
            .find_map(|line| line.strip_prefix("LLVM version: "))
            .context("expect `LLVM version` field")?
            .trim(),
    )?;

    if rustc_llvm_version < LLVM_MIN_VERSION || rustc_llvm_version >= LLVM_MAX_VERSION {
        bail!(Error::LLVMNotSupported(rustc_llvm_version))
    }

    // get llvm version from llvm-config with and without version suffix
    let config = ProcessBuilder::new("llvm-config")
        .arg("--version")
        .exec_with_output();
    let config_suffix = ProcessBuilder::new(format!("llvm-config-{}", rustc_llvm_version.major))
        .arg("--version")
        .exec_with_output();

    let comparator = Comparator {
        op: Op::Exact,
        major: rustc_llvm_version.major,
        minor: Some(rustc_llvm_version.minor),
        patch: None,
        pre: Default::default(),
    };

    // check if rustc and llvm are compatible and add version suffix if needed
    let add_suffix = match (config, config_suffix) {
        (Ok(out), Ok(out_suffix)) => {
            let llvm_version = Version::parse(String::from_utf8(out.stdout)?.trim())?;
            let llvm_version_suffix = Version::parse(String::from_utf8(out_suffix.stdout)?.trim())?;
            if comparator.matches(&llvm_version) {
                false
            } else if comparator.matches(&llvm_version_suffix) {
                true
            } else {
                bail!(Error::LLVMVersionNotMatch(rustc_llvm_version, llvm_version));
            }
        }
        (Ok(out), Err(_)) => {
            let llvm_version = Version::parse(String::from_utf8(out.stdout)?.trim())?;
            if !comparator.matches(&llvm_version) {
                bail!(Error::LLVMVersionNotMatch(rustc_llvm_version, llvm_version));
            }
            false
        }
        (Err(_), Ok(out_suffix)) => {
            let llvm_version_suffix = Version::parse(String::from_utf8(out_suffix.stdout)?.trim())?;
            if !comparator.matches(&llvm_version_suffix) {
                bail!(Error::LLVMVersionNotMatch(
                    rustc_llvm_version,
                    llvm_version_suffix
                ));
            }
            true
        }
        (Err(_), Err(_)) => {
            bail!(Error::LLVMNotInstalled);
        }
    };

    Ok(LlvmToolchain {
        version: rustc_llvm_version,
        suffix: add_suffix,
    })
}
