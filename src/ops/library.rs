use std::path::{Path, PathBuf};

use anyhow::{bail, Context};
use cargo_util::{paths, ProcessBuilder};
use tracing::info;

use crate::args::LibraryArgs;
use crate::config::Config;
use crate::error::CIError;
use crate::{util, CIResult};

pub fn exec(config: Config, args: LibraryArgs) -> CIResult<()> {
    if args.install {
        install(config, args)?;
    } else if args.uninstall {
        uninstall(config)?;
    } else if let Some(default_args) = args.args {
        set_default_args(config, default_args)?;
    } else {
        print_info(config)?;
    }

    Ok(())
}

fn install(mut config: Config, args: LibraryArgs) -> CIResult<()> {
    if Path::new(&config.library_path).is_file() {
        bail!(CIError::LibraryAlreadyInstalled(config.library_path));
    }

    let mut llvm_bins = ["llvm-config", "clang"]
        .iter()
        .map(|&s| s.to_string())
        .collect();
    let llvm_version = util::llvm_toolchain(&mut llvm_bins)?;

    println!("Installing the Compiler Interrupts library...");

    info!("getting the destination library path");
    let lib_path = {
        if let Some(apath) = args.path {
            // user-provided library path
            let mut path = PathBuf::from(&apath);
            if !path.is_dir() {
                bail!(CIError::PathNotDirectory(apath))
            }
            path.push("libci.so");
            if !path.exists() {
                paths::create_dir_all(&path)?;
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
    info!("lib_path: {}", lib_path);

    info!("getting option flags");
    let output = ProcessBuilder::new(&llvm_bins[0])
        .arg("--cxxflags")
        .exec_with_output()?;
    let cxx_flags = String::from_utf8(output.stdout)?;

    let output = ProcessBuilder::new(&llvm_bins[0])
        .arg("--ldflags")
        .exec_with_output()?;
    let ld_flags = String::from_utf8(output.stdout)?;

    let common_flags = "-O3 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC";

    let so_flags = match std::env::consts::OS {
        "macos" => "-bundle -undefined dynamic_lookup",
        _ => "-shared",
    };

    // source code path
    let mut src_path = std::env::temp_dir();
    src_path.push("ci.cpp");
    let src_path = src_path.to_str().unwrap();
    info!("src_path: {}", src_path);

    let repo_url = "https://raw.githubusercontent.com/quanshousio/CompilerInterrupts/\
                    main/src/CompilerInterrupt.cpp";
    info!("repo_url: {}", repo_url);

    // get the latest source code from the repo
    info!("running wget");
    let mut wget = ProcessBuilder::new("wget");
    wget.arg("--no-check-certificate");
    wget.args(&["-O", src_path, repo_url]);
    wget.exec_with_output()?;

    // compile
    info!("compiling the library");
    let mut clang = ProcessBuilder::new(&llvm_bins[1]);
    clang.arg(src_path);
    clang.args(&["-o".to_string(), lib_path.to_string()]);
    clang.args(so_flags.split_ascii_whitespace());
    clang.args(cxx_flags.split_ascii_whitespace());
    clang.args(ld_flags.split_ascii_whitespace());
    clang.args(common_flags.split_ascii_whitespace());
    info!("clang args: {:?}", clang.get_args());

    clang.exec().context("failed to compile the library")?;

    // update config
    info!("updating configuration");
    let default_args = [
        "-clock-type=1",
        "-config=2",
        "-inst-gran=2",
        "-all-dev=100",
        "-push-intv=1000",
        "-commit-intv=1000",
        "-mem-ops-cost=1",
    ]
    .iter()
    .map(|&s| s.to_string())
    .collect();

    config.library_path = lib_path.to_string();
    config.llvm_version = llvm_version;
    config.default_args = args.args.unwrap_or(default_args);

    if let Err(e) = Config::save(&config).context("failed to save the configuration") {
        // try to remove the library
        paths::remove_file(lib_path)?;
        return Err(e);
    }

    println!("Library has been successfully installed");
    print_info(config)?;

    Ok(())
}

fn uninstall(config: Config) -> CIResult<()> {
    // remove the library
    info!("uninstalling the library");
    if Path::new(&config.library_path).is_file() {
        paths::remove_file(config.library_path).context("failed to uninstall the library")?;
    } else {
        bail!(CIError::LibraryNotInstalled);
    }

    // update config
    info!("updating configuration");
    Config::save(&Config::default()).context("failed to save the configuration")?;

    println!("Compiler Interrupts library has been uninstalled");

    Ok(())
}

fn set_default_args(mut config: Config, default_args: Vec<String>) -> CIResult<()> {
    config.default_args = default_args;
    Config::save(&config).context("failed to save the configuration")?;

    println!("New default arguments of the library have been saved");
    println!("Default arguments: {}", config.default_args.join(" "));

    Ok(())
}

fn print_info(config: Config) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(CIError::LibraryNotInstalled);
    }

    println!("Path to the library: {}", config.library_path);
    println!("LLVM version: {}", config.llvm_version);
    println!("Default arguments: {}", config.default_args.join(" "));

    Ok(())
}
