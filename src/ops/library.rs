use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use cargo_util::{paths, ProcessBuilder};
use color_eyre::eyre::{bail, eyre, WrapErr};
use color_eyre::Help;
use faccess::PathExt;
use tracing::{debug, info};

use crate::args::LibraryArgs;
use crate::config::Config;
use crate::{util, CIResult};

pub fn exec(args: LibraryArgs) -> CIResult<()> {
    if args.install {
        install(args)?;
    } else if args.uninstall {
        uninstall()?;
    }

    Ok(())
}

fn install(args: LibraryArgs) -> CIResult<()> {
    // check if library is installed
    if let Ok(config) = Config::load() {
        if Path::new(&config.path).is_file() {
            return Err(eyre!(
                "Compiler Interrupts library has already been installed\n\
                Path to the library: {}",
                config.path
            )
            .suggestion(
                "If you want to reinstall, run `cargo lib-ci --uninstall` \
                to uninstall the library first",
            ));
        }
    }

    info!("getting the output path");
    let lib_path = {
        if let Some(path) = args.path {
            // user-provided path
            let path = PathBuf::from(&path);
            if !path.exists() {
                paths::create_dir_all(&path)?;
            }
            if !path.writable() {
                bail!("path: {} is not writable", path.display());
            }
            path
        } else {
            // default to config_path
            let mut path = util::config_path()?;
            path.push("libci.so");
            path
        }
    };
    let lib_path = lib_path.into_os_string().into_string().unwrap();

    println!("Installing the Compiler Interrupts library...");

    info!("getting version information");
    let output = ProcessBuilder::new("rustc")
        .arg("-vV")
        .exec_with_output()
        .wrap_err("failed to execute `rustc -vV`")?;
    let rustc_output = String::from_utf8(output.stdout)?;
    let rustc_llvm_version = rustc_output
        .lines()
        .filter_map(|line| line.strip_prefix("LLVM version: "))
        .next()
        .expect("`rustc -vV` should have the LLVM version field")
        .to_string();
    let rustc_llvm_version = rustc_llvm_version.trim();

    let output = ProcessBuilder::new("llvm-config")
        .arg("--version")
        .exec_with_output()
        .wrap_err("failed to execute `llvm-config --version`")?;
    let llvm_version = String::from_utf8(output.stdout)?;
    let llvm_version = llvm_version.trim();

    if llvm_version != rustc_llvm_version {
        bail!(
            "LLVM version from Rust toolchain ({}) does not \
            match with LLVM version from LLVM toolchain ({})",
            rustc_llvm_version,
            llvm_version
        );
    }
    info!("llvm version: {}", llvm_version);

    info!("getting option flags");
    let output = ProcessBuilder::new("llvm-config")
        .arg("--cxxflags")
        .exec_with_output()
        .wrap_err("failed to execute `llvm-config --cxxflags`")?;
    let cxx_flags = String::from_utf8(output.stdout)?;

    let output = ProcessBuilder::new("llvm-config")
        .arg("--ldflags")
        .exec_with_output()
        .wrap_err("failed to execute `llvm-config --ldflags`")?;
    let ld_flags = String::from_utf8(output.stdout)?;

    let common_flags = "-O3 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC";

    let so_flags = match env::consts::OS {
        "macos" => "-bundle -undefined dynamic_lookup",
        _ => "-shared",
    };

    // source code path
    let mut src_path = env::temp_dir();
    src_path.push("ci.cpp");
    let src_path = src_path.to_str().unwrap();
    info!("source code path: {}", src_path);

    let repo_url = "https://raw.githubusercontent.com/quanshousio/CompilerInterrupts/main/src/CompilerInterrupt.cpp";
    info!("repo url: {}", repo_url);

    // get the latest source code from the repo
    let mut wget = ProcessBuilder::new("wget");
    wget.args(["-O", src_path, repo_url]);
    let output = wget.exec_with_output()?;
    info!("wget output: {:?}", output);

    // compile
    info!("compiling the library");
    let mut cpp = ProcessBuilder::new("c++");
    cpp.arg(src_path);
    cpp.args(&["-o".to_string(), lib_path.to_string()]);
    cpp.args(so_flags.split_ascii_whitespace());
    cpp.args(cxx_flags.split_ascii_whitespace());
    cpp.args(ld_flags.split_ascii_whitespace());
    cpp.args(common_flags.split_ascii_whitespace());
    debug!("args: {:?}", cpp.get_args());

    cpp.exec().wrap_err("Failed to compile the library\nPlease report the bug at https://github.com/bitslab/cargo-ci")?;

    // update config
    let config = Config {
        path: lib_path.to_string(),
        llvm_version: llvm_version.to_string(),
        args: args.args,
    };
    if let Err(e) = config.save() {
        fs::remove_file(lib_path)?;
        fs::remove_file(src_path)?;
        return Err(e);
    }

    println!("Successfully installed to {}", lib_path);

    Ok(())
}

fn uninstall() -> CIResult<()> {
    // check if library is installed
    let mut config = Config::load()
        .wrap_err("Compiler Interrupts library is not installed")
        .suggestion("Run `cargo lib-ci --install` to install the library")?;

    // remove the library
    if Path::new(&config.path).exists() {
        fs::remove_file(&config.path)
            .wrap_err(format!(
                "Failed to uninstall the library\nPath to the library: {}",
                config.path
            ))
            .suggestion("Try delete the library manually")?;
    } else {
        return Err(eyre!("Compiler Interrupts library is not installed")
            .suggestion("Run `cargo lib-ci --install` to install the library"));
    }

    // update config
    config.path = String::new();
    config.llvm_version = String::new();
    config.args = Vec::new();
    config.save().wrap_err("Failed to save the configuration")?;

    println!("Compiler Interrupts library has been uninstalled");

    Ok(())
}
