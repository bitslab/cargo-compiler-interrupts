//! Implementation of `cargo-lib-ci`.

use std::io::Read;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context};
use cargo_util::{paths, ProcessBuilder};
use colored::Colorize;
use indicatif::{ProgressBar, ProgressStyle};
use tracing::{debug, info};

use crate::config::Config;
use crate::error::CIError;
use crate::opts::LibraryOpts;
use crate::{util, CIResult};

/// Main routine for `cargo-lib-ci`.
pub fn exec(config: Config, opts: LibraryOpts) -> CIResult<()> {
    if opts.install {
        install(config, opts, false)?;
    } else if opts.uninstall {
        uninstall(config)?;
    } else if opts.update {
        install(config, opts, true)?;
    } else if let Some(default_args) = opts.args {
        set_default_args(config, default_args)?;
    } else {
        print_info(config)?;
    }

    Ok(())
}

/// Installs the Compiler Interrupts library.
fn install(mut config: Config, opts: LibraryOpts, update: bool) -> CIResult<()> {
    let lib_exists = Path::new(&config.library_path).is_file();
    if !update && lib_exists {
        bail!(CIError::LibraryAlreadyInstalled(config.library_path));
    } else if update && !lib_exists {
        bail!(CIError::LibraryNotInstalled)
    }

    // let's go
    let time = std::time::Instant::now();

    // progress bar
    let pb = if opts.verbose == 0 {
        ProgressBar::new_spinner()
    } else {
        ProgressBar::hidden()
    };
    let ps = ProgressStyle::default_spinner()
        .tick_chars("/|\\- ")
        .template("{spinner:.dim.bold} {prefix:>10.cyan.bold} {wide_msg}");
    pb.enable_steady_tick(200);
    pb.set_style(ps);
    if update {
        pb.set_prefix("Updating");
    } else {
        pb.set_prefix("Installing");
    }

    if update {
        pb.set_message("Checking for update");
    } else {
        pb.set_message("Fetching the source code");
    }

    // fetch the source code
    let mut src_path = std::env::temp_dir();
    src_path.push("CompilerInterrupt.cpp");
    info!("src_path: {}", src_path.display());

    let url = if update {
        config.url.clone()
    } else {
        let default_url = "https://raw.githubusercontent.com/bitslab/\
            CompilerInterrupts/main/src/CompilerInterrupt.cpp";
        opts.url.unwrap_or_else(|| default_url.to_string())
    };
    let resp = ureq::get(&url).call()?;
    let len = resp
        .header("Content-Length")
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(1_048_576);
    let mut bytes = Vec::with_capacity(len);
    resp.into_reader().read_to_end(&mut bytes)?;
    paths::write(&src_path, &bytes)?;
    let digest = md5::compute(&bytes);
    let checksum = format!("{:x}", digest);
    info!("checksum: {}", checksum);

    if update && config.checksum == checksum {
        pb.finish_and_clear();
        println!(
            "{:>12} Compiler Interrupts library is up-to-date",
            "Finished".green().bold()
        );
        return Ok(());
    }

    pb.set_message("Checking the compiler toolchain");

    info!("checking llvm binaries");
    let mut llvm_bins = ["llvm-config", "clang"]
        .iter()
        .map(|&s| s.to_string())
        .collect();
    let llvm_version = util::llvm_toolchain(&mut llvm_bins)?;
    let llvm_major_version = llvm_version
        .split('.')
        .next()
        .context("llvm version string is not valid")?;

    let llvm_config = &llvm_bins[0];
    let clang = &llvm_bins[1];

    info!("getting option flags");
    let output = ProcessBuilder::new(llvm_config)
        .arg("--cxxflags")
        .exec_with_output()?;
    let cxx_flags = String::from_utf8(output.stdout)?;
    debug!("cxx_flags: {}", cxx_flags);

    let output = ProcessBuilder::new(llvm_config)
        .arg("--ldflags")
        .exec_with_output()?;
    let ld_flags = String::from_utf8(output.stdout)?;
    debug!("ld_flags: {}", ld_flags);

    let common_flags = "-O3 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC";

    let so_flags = match std::env::consts::OS {
        "macos" => "-bundle -undefined dynamic_lookup",
        _ => "-shared",
    };

    let llvm_version_macro = match llvm_major_version {
        "12" => "-DLLVM12",
        "11" => "-DLLVM11",
        "10" => "-DLLVM10",
        "9" => "-DLLVM9",
        _ => "",
    };

    info!("getting the destination library path");
    let file_name = format!("CompilerInterrupt-{:x}.so", digest);
    let lib_path = {
        if !config.library_path.is_empty() {
            PathBuf::from(&config.library_path)
        } else if let Some(args_path) = opts.path {
            // user-provided library path
            let mut path = PathBuf::from(&args_path);
            if !path.is_dir() {
                bail!(CIError::PathNotDirectory(args_path))
            }
            path.push(file_name);
            if !path.exists() {
                paths::create_dir_all(&path)?;
            }
            path
        } else {
            // default to config_path
            let mut path = util::config_path()?;
            path.push(file_name);
            path
        }
    };
    info!("lib_path: {}", lib_path.display());

    let lib_path = util::path_to_string(&lib_path);
    let lib_path_dbg = util::path_to_string(util::append_suffix(&lib_path, "dbg"));

    pb.set_message("Compiling the Compiler Interrupts library");

    // compile
    let mut clang = ProcessBuilder::new(clang);
    clang.arg("-fdiagnostics-color=always");
    clang.arg(src_path);
    clang.arg(llvm_version_macro);
    clang.args(&so_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&cxx_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&ld_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&common_flags.split_ascii_whitespace().collect::<Vec<_>>());
    debug!("clang_base_args: {:?}", clang.get_args());

    let mut clang_dbg = clang.clone();

    info!("compiling the library");
    clang.args(&["-o".to_string(), lib_path.clone()]);
    clang
        .exec_with_streaming(
            &mut |out| {
                pb.println(out);
                Ok(())
            },
            &mut |err| {
                pb.println(err);
                Ok(())
            },
            false,
        )
        .context("Failed to compile the library")?;

    info!("compiling the library with debugging mode");
    pb.set_message("Compiling the Compiler Interrupts library with debugging mode");

    clang_dbg.arg("-DDBG_DETAILED");
    clang_dbg.args(&["-o".to_string(), lib_path_dbg.clone()]);
    clang_dbg
        .exec_with_streaming(
            &mut |out| {
                pb.println(out);
                Ok(())
            },
            &mut |err| {
                pb.println(err);
                Ok(())
            },
            false,
        )
        .context("Failed to compile the library with debugging mode")?;

    // update config
    info!("updating configuration");
    let default_args = ["-inst-gran=2", "-commit-intv=100", "-all-dev=100"]
        .iter()
        .map(|&s| s.to_string())
        .collect();

    config.library_path = lib_path.clone();
    config.library_path_dbg = lib_path_dbg;
    config.llvm_version = llvm_version;
    config.checksum = checksum;
    if !update {
        config.default_args = opts.args.unwrap_or(default_args);
        config.url = url;
    }

    if let Err(e) = Config::save(&config).context("failed to save the configuration") {
        // try to remove the library
        paths::remove_file(&lib_path)?;
        return Err(e);
    }

    pb.finish_and_clear();
    println!(
        "{:>12} Compiler Interrupts library has been successfully {} in {}",
        "Finished".green().bold(),
        if update { "updated" } else { "installed" },
        util::human_duration(time.elapsed())
    );
    print_info(config)?;

    Ok(())
}

/// Uninstalls the Compiler Interrupts library.
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

    println!(
        "{:>12} Compiler Interrupts library has been uninstalled",
        "Finished".green().bold(),
    );

    Ok(())
}

/// Sets the default arguments for the library.
fn set_default_args(mut config: Config, default_args: Vec<String>) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(CIError::LibraryNotInstalled)
    }

    config.default_args = default_args;
    Config::save(&config).context("failed to save the configuration")?;

    println!(
        "{:>12} New default arguments of the library have been saved",
        "Finished".green().bold(),
    );

    Ok(())
}

/// Outputs the configuration about the library.
fn print_info(config: Config) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(CIError::LibraryNotInstalled);
    }

    println!("Path to the library: {}", config.library_path);
    println!("LLVM version: {}", config.llvm_version);
    println!("Default arguments: {}", config.default_args.join(" "));
    println!("Checksum: {}", config.checksum);
    println!("Remote URL: {}", config.url);

    Ok(())
}
