//! Implementation of `cargo-lib-ci`.

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::time::Duration;

use anyhow::{bail, Context};
use cargo_util::{paths, ProcessBuilder};
use clap::Parser;
use colored::Colorize;
use indicatif::{ProgressBar, ProgressStyle};
use tracing::{debug, info, Level};
use url::Url;

use crate::args::{ConfigArgs, InstallArgs, LibraryArgs, LibrarySubcommands::*};
use crate::config::Config;
use crate::error::Error;
use crate::llvm::{LlvmToolchain, LlvmUtility};
use crate::paths::PathExt;
use crate::{llvm, util, CIResult, LIB_CI_BIN_NAME};

/// Default URL for the Compiler Interrupts source code.
const DEFAULT_CI_URL: &str = "https://raw.githubusercontent.com/bitslab/\
    CompilerInterrupts/main/src/CompilerInterrupt.cpp";

/// Default arguments for the Compiler Interrupts library.
const DEFAULT_CI_ARGS: [&str; 3] = ["-inst-gran=2", "-commit-intv=100", "-all-dev=100"];

/// Main routine for `cargo-lib-ci`.
pub fn exec() -> CIResult<()> {
    let args = if std::env::args().next().unwrap_or_default() == LIB_CI_BIN_NAME {
        LibraryArgs::parse()
    } else {
        LibraryArgs::parse_from(std::env::args().skip(1))
    };

    util::init_logger(&args.log_level)?;

    let config = Config::load()?;
    let toolchain = llvm::toolchain()?;

    _exec(config, args, toolchain)
}

/// Core routine for `cargo-lib-ci`.
fn _exec(config: Config, args: LibraryArgs, toolchain: LlvmToolchain) -> CIResult<()> {
    if let Some(command) = &args.command {
        match command {
            Install(install_args) => install(config, &args, install_args, &toolchain)?,
            Uninstall => uninstall(config)?,
            Update => update(config, &args, &toolchain)?,
            Config(config_args) => configure(config, config_args)?,
        }
    } else {
        print_info(&config)?;
    }

    Ok(())
}

/// Installs the Compiler Interrupts library.
fn install(
    mut config: Config,
    args: &LibraryArgs,
    install_args: &InstallArgs,
    toolchain: &LlvmToolchain,
) -> CIResult<()> {
    if Path::new(&config.library_path).is_file() {
        bail!(Error::LibraryAlreadyInstalled);
    }

    let time = std::time::Instant::now();

    // progress bar
    let pb = if Level::from_str(&args.log_level)? != Level::DEBUG {
        ProgressBar::new_spinner()
    } else {
        ProgressBar::hidden()
    };
    let ps = ProgressStyle::with_template("{spinner:.dim.bold} {prefix:>10.cyan.bold} {wide_msg}")?
        .tick_chars("/|\\- ");
    pb.enable_steady_tick(Duration::from_millis(200));
    pb.set_style(ps);
    pb.set_prefix("Installing");

    pb.set_message("Fetching the source code");

    info!("fetching the source code");
    let url = Url::parse(
        &install_args
            .url
            .clone()
            .unwrap_or_else(|| DEFAULT_CI_URL.to_string()),
    )?;
    let src_code = fetch_source_code(&url)?;

    let src_dir = std::env::temp_dir()
        .join("CompilerInterrupt.cpp")
        .to_string()?;
    info!(?src_dir);

    paths::write(&src_dir, &src_code).context("failed to save the library")?;
    let checksum = format!("{:x}", md5::compute(&src_code));
    info!(?checksum);

    info!("getting the destination library path");
    let library_path = {
        let file_name = format!("CompilerInterrupt-{}.so", checksum);
        if let Some(args_path) = &install_args.path {
            // user-provided library path
            let mut path = PathBuf::from(args_path);
            path.push(file_name);
            if !path.exists() {
                paths::create_dir_all(&path)?;
            }
            path
        } else {
            let mut path = Config::dir()?;
            path.push(file_name);
            path
        }
    };
    info!(?library_path);

    let out_dir = library_path.to_string()?;
    let out_debug_dir = library_path.append_suffix("debug")?.to_string()?;

    info!("getting the compiler config");
    pb.set_message("Getting the compiler configuration");
    let clang = compiler(toolchain)?;
    // debug!("clang_args: {:?}", clang.get_args());

    info!("compiling the library");
    pb.set_message("Compiling the Compiler Interrupts library");
    compile(clang.clone(), &src_dir, &out_dir, false, &pb)?;

    info!("compiling the library with debugging mode");
    pb.set_message("Compiling the Compiler Interrupts library with debugging mode");
    compile(clang, &src_dir, &out_debug_dir, true, &pb)?;

    // update config
    info!("updating configuration");
    config.library_path = PathBuf::from(&out_dir);
    config.library_debug_path = PathBuf::from(&out_debug_dir);
    config.library_args = DEFAULT_CI_ARGS.iter().map(|&s| s.to_string()).collect();
    config.llvm_version = toolchain.version.to_string();
    config.checksum = checksum;
    config.url = url.to_string();

    Config::save(&config)?;

    pb.finish_and_clear();

    print_info(&config)?;

    println!(
        "{:>12} Compiler Interrupts library has been installed in {}",
        "Finished".green().bold(),
        util::human_duration(time.elapsed())
    );

    Ok(())
}

/// Uninstalls the Compiler Interrupts library.
fn uninstall(config: Config) -> CIResult<()> {
    // remove the library
    info!("uninstalling the library");
    if Path::new(&config.library_path).is_file() {
        paths::remove_file(config.library_path).context("failed to uninstall the library")?;
    } else {
        bail!(Error::LibraryNotInstalled);
    }

    // update config
    info!("updating configuration");
    Config::save(&Config::default())?;

    println!(
        "{:>12} Compiler Interrupts library has been uninstalled",
        "Finished".green().bold(),
    );

    Ok(())
}

/// Updates the Compiler Interrupts library.
fn update(mut config: Config, args: &LibraryArgs, toolchain: &LlvmToolchain) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(Error::LibraryAlreadyInstalled);
    }

    let time = std::time::Instant::now();

    // progress bar
    let pb = if Level::from_str(&args.log_level)? != Level::DEBUG {
        ProgressBar::new_spinner()
    } else {
        ProgressBar::hidden()
    };
    let ps = ProgressStyle::with_template("{spinner:.dim.bold} {prefix:>10.cyan.bold} {wide_msg}")?
        .tick_chars("/|\\- ");
    pb.enable_steady_tick(Duration::from_millis(200));
    pb.set_style(ps);
    pb.set_prefix("Updating");

    pb.set_message("Checking for update");

    info!("fetching the source code");
    let url = Url::parse(&config.url)?;
    let src_code = fetch_source_code(&url)?;

    let src_dir = std::env::temp_dir()
        .join("CompilerInterrupt.cpp")
        .to_string()?;
    info!(?src_dir);

    paths::write(&src_dir, &src_code).context("failed to save the library")?;
    let checksum = format!("{:x}", md5::compute(&src_code));
    info!(?checksum);

    if config.checksum == checksum {
        pb.finish_and_clear();
        println!(
            "{:>12} Compiler Interrupts library is up-to-date",
            "Finished".green().bold()
        );
        return Ok(());
    }

    info!("getting the destination library path");
    let library_path = {
        let file_name = format!("CompilerInterrupt-{}.so", checksum);
        if config.library_path.is_file() {
            config.library_path
        } else {
            let mut path = Config::dir()?;
            path.push(file_name);
            path
        }
    };
    info!(?library_path);

    let out_dir = library_path.to_string()?;
    let out_debug_dir = library_path.append_suffix("debug")?.to_string()?;

    pb.set_message("Compiling the Compiler Interrupts library");

    // compile
    info!("getting the compiler config");
    let clang = compiler(toolchain)?;

    info!("compiling the library");
    pb.set_message("Compiling the Compiler Interrupts library");
    compile(clang.clone(), &src_dir, &out_dir, false, &pb)?;

    info!("compiling the library with debugging mode");
    pb.set_message("Compiling the Compiler Interrupts library with debugging mode");
    compile(clang, &src_dir, &out_debug_dir, true, &pb)?;

    // update config
    info!("updating configuration");
    config.library_path = PathBuf::from(&out_dir);
    config.library_debug_path = PathBuf::from(&out_debug_dir);
    config.llvm_version = toolchain.version.to_string();
    config.checksum = checksum;

    Config::save(&config)?;

    pb.finish_and_clear();

    print_info(&config)?;

    println!(
        "{:>12} Compiler Interrupts library has been updated in {}",
        "Finished".green().bold(),
        util::human_duration(time.elapsed())
    );

    Ok(())
}

/// Configures the Compiler Interrupts library.
fn configure(mut config: Config, config_args: &ConfigArgs) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(Error::LibraryNotInstalled);
    }

    info!("configuring the library");

    if let Some(library_args) = &config_args.library_args {
        debug!(?library_args);
        config.library_args = library_args.clone();
    }

    Config::save(&config)?;

    print_info(&config)?;

    println!(
        "{:>12} New library configuration has been saved",
        "Finished".green().bold(),
    );

    Ok(())
}

/// Outputs the configuration about the library.
fn print_info(config: &Config) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(Error::LibraryNotInstalled);
    }

    println!("Library path: {}", config.library_path.display());
    println!("Library arguments: {}", config.library_args.join(" "));
    println!("LLVM version: {}", config.llvm_version);
    println!("Checksum: {}", config.checksum);
    println!("URL: {}", config.url);

    Ok(())
}

/// Fetch the source code given the URL.
fn fetch_source_code(url: &Url) -> CIResult<Vec<u8>> {
    if let Ok(path) = url.to_file_path() {
        Ok(fs::read(path)?)
    } else {
        let resp = ureq::get(url.as_str()).call()?;
        let len = resp
            .header("Content-Length")
            .and_then(|s| s.parse::<usize>().ok())
            .unwrap_or(1_048_576);
        let mut src_code = Vec::with_capacity(len);
        resp.into_reader().read_to_end(&mut src_code)?;
        Ok(src_code)
    }
}

/// Get the compiler with required arguments.
fn compiler(toolchain: &LlvmToolchain) -> CIResult<ProcessBuilder> {
    let output = LlvmUtility::Config
        .process_builder(toolchain)
        .arg("--cxxflags")
        .exec_with_output()?;
    let cxx_flags = String::from_utf8(output.stdout)?;
    debug!(?cxx_flags);

    let output = LlvmUtility::Config
        .process_builder(toolchain)
        .arg("--ldflags")
        .exec_with_output()?;
    let ld_flags = String::from_utf8(output.stdout)?;
    debug!(?ld_flags);

    let common_flags = "-O3 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC";

    let so_flags = if cfg!(target_os = "macos") {
        "-bundle -undefined dynamic_lookup"
    } else {
        "-shared"
    };

    let mut clang = LlvmUtility::Clang.process_builder(toolchain);
    clang.args(&so_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&cxx_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&ld_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.args(&common_flags.split_ascii_whitespace().collect::<Vec<_>>());
    clang.arg("-fdiagnostics-color=always");
    clang.arg(format!("-DLLVM{}", toolchain.version.major));

    Ok(clang)
}

/// Compile the library.
fn compile<P: AsRef<Path>>(
    mut clang: ProcessBuilder,
    input: P,
    output: P,
    debug: bool,
    pb: &ProgressBar,
) -> CIResult<()> {
    if debug {
        clang.arg("-DDBG_DETAILED");
    }
    clang.arg(input.as_ref());
    clang.arg("-o");
    clang.arg(output.as_ref());
    debug!(?clang);
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
        .context("failed to compile the library")?;

    Ok(())
}
