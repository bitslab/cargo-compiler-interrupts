//! Implementation of `cargo-build-ci`.

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::process::Output;
use std::sync::{mpsc, Arc, Mutex};

use anyhow::{bail, Context};
use cargo_metadata::{Metadata, MetadataCommand};
use cargo_util::{paths, ProcessBuilder, ProcessError};
use colored::Colorize;
use crossbeam_utils::thread;
use faccess::PathExt;
use indicatif::{ProgressBar, ProgressStyle};
use tracing::{debug, info};

use crate::config::Config;
use crate::error::CIError;
use crate::opts::BuildOpts;
use crate::{util, CIResult};

/// State of the integration.
#[derive(Debug)]
enum State {
    /// Running `opt`.
    Opt(bool),
    /// Running `llc`.
    Llc(bool),
    /// Running linker.
    Ld(bool),
    /// Crate is skipped.
    Skipped,
    /// An error occurred.
    Error(String),
}

/// Shared context of the integration.
#[derive(Debug)]
struct IntegrationCx {
    /// Name of the crate.
    crate_name: Arc<String>,
    /// Current state.
    state: State,
}

/// Linker invocation.
#[derive(Debug)]
struct Linker {
    /// Linker program name.
    program: String,
    /// Arguments for the linker.
    args: Vec<String>,
    /// Path to the target binary.
    bin_path: String,
}

/// Main routine for `cargo-build-ci`.
pub fn exec(config: Config, opts: BuildOpts) -> CIResult<()> {
    if let Err(e) = _exec(&config, &opts) {
        // make the build dirty if the integration failed
        let target_path = util::target_path(&opts.target, &opts.release)?;
        let deps_path = target_path.join("deps");
        let examples_path = target_path.join("examples");
        let binary_deps_files =
            util::scan_path(&deps_path, |p| p.executable() && p.is_file()).unwrap_or_default();
        let binary_examples_files =
            util::scan_path(&examples_path, |p| p.executable() && p.is_file()).unwrap_or_default();
        for file in binary_deps_files.iter().chain(binary_examples_files.iter()) {
            paths::remove_file(file)?;
        }
        return Err(e);
    }

    Ok(())
}

/// Core routine for `cargo-build-ci`.
fn _exec(config: &Config, opts: &BuildOpts) -> CIResult<()> {
    if !Path::new(&config.library_path).is_file() {
        bail!(CIError::LibraryNotInstalled);
    }

    let mut llvm_bins = ["opt", "llc", "llvm-ar", "llvm-nm"]
        .iter()
        .map(|&s| s.to_string())
        .collect();
    util::llvm_toolchain(&mut llvm_bins)?;

    let opt = &llvm_bins[0];
    let llc = &llvm_bins[1];
    let ar = &llvm_bins[2];
    let nm = &llvm_bins[3];

    // get all binary-type crate names, including examples
    let mut crate_names = Vec::new();
    let metadata = cargo_metadata()?;
    for package in metadata.packages {
        for target in package.targets {
            if target.crate_types.iter().any(|t| t == "bin") {
                crate_names.push(target.name.replace("-", "_"));
            }
        }
    }
    debug!("crate_names: {:#?}", crate_names);

    if crate_names.is_empty() {
        bail!(CIError::BinaryNotFound);
    }

    if opts.debug_ci {
        println!("{:>12} Debugging mode is enabled", "Note".cyan().bold());
    }

    let mut mtimes = HashMap::new();
    let mut stale_files = Vec::new();

    let target_path = util::target_path(&opts.target, &opts.release)?;
    let deps_path = target_path.join("deps");
    let examples_path = target_path.join("examples");

    // get timestamp from output files before running `cargo build`
    let deps_files = util::scan_path(&deps_path, |p| p.is_file()).unwrap_or_default();
    for file in deps_files {
        let mtime = paths::mtime(&file)?;
        assert!(mtimes.insert(file, mtime).is_none());
    }
    let examples_files = util::scan_path(&examples_path, |p| p.is_file()).unwrap_or_default();
    for file in examples_files {
        let mtime = paths::mtime(&file)?;
        assert!(mtimes.insert(file, mtime).is_none());
    }

    // run `cargo build`
    let cargo_build = cargo_build(&opts)?;

    // let's go
    let time = std::time::Instant::now();

    // check for stale files after the compilation
    let deps_files = util::scan_path(&deps_path, |p| p.is_file())?;
    for file in &deps_files {
        let new_mtime = paths::mtime(&file)?;
        if let Some(cache_mtime) = mtimes.get(file) {
            if new_mtime > *cache_mtime {
                stale_files.push(file);
            }
        } else {
            mtimes.insert(file.clone(), new_mtime);
            stale_files.push(file);
        }
    }

    let examples_files = util::scan_path(&examples_path, |p| p.is_file()).unwrap_or_default();
    for file in &examples_files {
        let new_mtime = paths::mtime(&file)?;
        if let Some(cache_mtime) = mtimes.get(file) {
            if new_mtime > *cache_mtime {
                stale_files.push(file);
            }
        } else {
            mtimes.insert(file.clone(), new_mtime);
            stale_files.push(file);
        }
    }

    debug!("stale_files: {:#?}", stale_files);

    if stale_files.is_empty() {
        println!(
            "{:>12} nothing to integrate, all fresh",
            "Finished".green().bold(),
        );
        return Ok(());
    }

    // name of stale crates
    let stale_crate_names = stale_files
        .iter()
        .filter(|p| crate_names.contains(&crate_name(p)))
        .map(crate_name)
        .collect::<HashSet<_>>();
    debug!("stale_crate_names: {:#?}", stale_crate_names);

    // *.rcgu.ll are intermediate files generated by `rustc -C save-temps`
    let ll_files = deps_files
        .iter()
        .chain(examples_files.iter())
        .filter(|p| {
            util::file_stem(p).contains("rcgu")
                && !util::file_stem(p).contains("-ci")
                && util::extension(p) == "ll"
        })
        .collect::<Vec<_>>();

    // parse cargo build output to get the linker invocation
    let iter = cargo_build.iter();
    let mut linkers = Vec::new();
    'outer: for info in iter {
        if !info.contains("libcompiler_builtins") {
            // ignore as this is not a linker invocation
            continue;
        }

        let linker = info
            .replace("\"", "")
            .split_ascii_whitespace()
            .skip(2) // skip "INFO", "rustc_codegen_ssa::back::link"
            .map(str::to_string)
            .collect::<Vec<_>>();

        let mut iter = linker.into_iter();
        let program = iter.next().context("expected linker program name")?;
        let args = iter.clone().collect::<Vec<_>>();
        let mut bin_path = String::new();
        while let Some(arg) = iter.next() {
            if arg.contains("-o") {
                bin_path = iter.next().context("expected path to binary")?;
                let crate_name = crate_name(&bin_path);

                if !stale_crate_names.contains(&crate_name) {
                    // redundant linker as the binary is still fresh
                    debug!("skipped linker invocation for binary \"{}\"", crate_name);
                    continue 'outer;
                }
            }
        }

        if bin_path.is_empty() {
            debug!("bin_path is empty while parsing the linker, skipped");
            continue;
        }

        linkers.push(Linker {
            program,
            args,
            bin_path,
        });
    }

    // total length of the process bar
    let len = ll_files.len() * 2 + linkers.len() + 1;

    let crate_names = &crate_names;
    let deps_path = &deps_path;

    let ll_iter = Arc::new(Mutex::new(ll_files.iter()));
    let lk_iter = Arc::new(Mutex::new(linkers.iter_mut()));

    thread::scope(move |s| -> CIResult<()> {
        // communication between the progress bar thread and integration threads
        let (tx, rx) = mpsc::channel::<IntegrationCx>();

        // number of threads based on number of logical cores in CPU
        let num_cpus = num_cpus::get();

        // handle progress bar rendering
        let pb_thread = s.spawn(move |_| {
            // progress bar
            let pb = if opts.verbose == 0 {
                ProgressBar::new(len as u64)
            } else {
                ProgressBar::hidden()
            };
            pb.set_prefix("Building");

            let mut names: Vec<String> = Vec::new();
            let mut error = false;

            while let Ok(integration) = rx.recv() {
                if error {
                    continue;
                }

                let name = integration.crate_name;
                let status_line =
                    |status: &str| -> String { format!("{:>12} {}", status.green().bold(), name) };
                let mut remove = |name: &String| {
                    let idx = names
                        .iter()
                        .position(|e| e == name)
                        .expect("failed to find crate name");
                    names.remove(idx);
                };

                match integration.state {
                    State::Opt(finished) => {
                        if finished {
                            remove(&name);
                        } else {
                            pb.println(status_line("Integrating"));
                            pb.inc(1);
                            names.insert(0, name.to_string());
                        }
                    }
                    State::Llc(finished) => {
                        let llc_name = format!("{}(llc)", name);
                        if finished {
                            remove(&llc_name);
                        } else {
                            pb.inc(1);
                            names.insert(0, llc_name);
                        }
                    }
                    State::Ld(finished) => {
                        let ld_name = format!("{}(bin)", name);
                        if finished {
                            remove(&ld_name);
                        } else {
                            pb.println(status_line("Linking"));
                            pb.inc(1);
                            names.insert(0, ld_name);
                        }
                    }
                    State::Skipped => {
                        // redundant to print `compiler_interrupts` status as it is always skipped
                        if *name != "compiler_interrupts" {
                            pb.println(status_line("Skipped"));
                        }
                        pb.inc(1);
                    }
                    State::Error(msg) => {
                        pb.set_style(
                            ProgressStyle::default_bar().template("{prefix:>12.red.bold} {msg}"),
                        );
                        pb.set_prefix("Error");
                        pb.finish_with_message(
                            "Compiler Interrupts integration has unexpectedly failed",
                        );
                        println!("{:>12} {}", "Warning".yellow().bold(), msg);

                        // we must not prematurely close the channel
                        // channel must live until all threads are done sending signals
                        error = true;
                        continue;
                    }
                }

                // progress bar message
                let term_size = terminal_size::terminal_size()
                    .map(|(w, h)| (w.0.into(), h.0.into()))
                    .unwrap_or((80, 24));
                let prefix_size = if term_size.0 > 80 { 50 } else { 20 };
                let template = if term_size.0 > 80 {
                    "{prefix:>12.cyan.bold} [{bar:27}] {pos}/{len}: {wide_msg}"
                } else {
                    "{prefix:>12.cyan.bold} {pos}/{len}: {wide_msg}"
                };
                let mut msg = String::new();
                let mut iter = names.iter();
                let first = match iter.next() {
                    Some(first) => first,
                    None => "",
                };
                msg.push_str(first);
                for name in iter {
                    msg.push_str(", ");
                    // truncate the message if too wide
                    if msg.len() + name.len() < term_size.0 - prefix_size - /* padding */ 15 {
                        msg.push_str(name);
                    } else {
                        msg.push_str("...");
                        break;
                    }
                }
                pb.set_style(
                    ProgressStyle::default_bar()
                        .template(template)
                        .progress_chars("=> "),
                );
                pb.set_message(msg);
            }

            if !error {
                pb.inc(1);
                pb.finish_and_clear();
            }
        });

        // integration
        let mut threads = Vec::new();
        for _ in 0..num_cpus {
            let tx = tx.clone();
            let iter = Arc::clone(&ll_iter);
            let thread = s.spawn(move |_| -> CIResult<()> {
                loop {
                    let file = iter.lock().expect("failed to acquire lock").next();
                    if let Some(file) = file {
                        let mut integrate = true;
                        let crate_name = Arc::new(crate_name(&file));
                        let ci_file = util::append_suffix(&file, "ci");

                        // `nm -jU` displays defined symbol names
                        let output = ProcessBuilder::new(nm)
                            .arg("-jU")
                            .arg(file.with_extension("o"))
                            .exec_with_output()?;
                        let stdout = String::from_utf8(output.stdout)?;
                        if stdout.contains("intvActionHook") {
                            // skip the `compiler-interrupts` crate
                            integrate = false;
                        }
                        if let Some(skip_crates) = &opts.skip_crates {
                            for skip_crate in skip_crates {
                                if skip_crate.replace("-", "_").contains(&*crate_name) {
                                    // skip the given crates
                                    integrate = false;
                                    break;
                                }
                            }
                        }

                        if integrate {
                            info!("integrating: {}", file.display());
                            tx.send(IntegrationCx {
                                crate_name: Arc::clone(&crate_name),
                                state: State::Opt(false),
                            })?;

                            // define `LocalLC` if it is a binary target
                            let def_clock = if crate_names.contains(&crate_name.to_string()) {
                                "-defclock=1"
                            } else {
                                "-defclock=0"
                            };

                            // `opt` runs the integration
                            let output = ProcessBuilder::new(opt)
                                .args(&[
                                    "-S",
                                    "-load",
                                    &config.library_path,
                                    "-logicalclock",
                                    def_clock,
                                ])
                                .args(&config.default_args)
                                .arg(&file)
                                .arg("-o")
                                .arg(&ci_file)
                                .exec_with_output();
                            handle_output(output, &ci_file, &tx, opts.debug_ci)?;

                            tx.send(IntegrationCx {
                                crate_name: Arc::clone(&crate_name),
                                state: State::Opt(true),
                            })?;
                        } else {
                            info!("integration skipped: {}", file.display());
                            tx.send(IntegrationCx {
                                crate_name: Arc::clone(&crate_name),
                                state: State::Skipped,
                            })?;
                            paths::copy(&file, &ci_file)?;
                        }

                        // `llc` transforms integrated IR bitcode to object file
                        debug!("run llc on: {}", ci_file.display());
                        tx.send(IntegrationCx {
                            crate_name: Arc::clone(&crate_name),
                            state: State::Llc(false),
                        })?;

                        let mut llc = ProcessBuilder::new(llc);
                        llc.arg("-filetype=obj");
                        llc.arg(&ci_file);

                        // `-code-model=large` fixes mismatch relocation symbols on Linux
                        if std::env::consts::OS == "linux" {
                            llc.arg("-code-model=large");
                        }

                        let output = llc.exec_with_output();
                        handle_output(output, &ci_file, &tx, opts.debug_ci)?;

                        tx.send(IntegrationCx {
                            crate_name: Arc::clone(&crate_name),
                            state: State::Llc(true),
                        })?;
                    } else {
                        break;
                    }
                }

                Ok(())
            });
            threads.push(thread);
        }
        for thread in threads {
            thread
                .join()
                .expect("integration thread panicked")
                .context("integration failed")?;
        }

        // linking
        let mut threads = Vec::new();
        for _ in 0..num_cpus {
            let tx = tx.clone();
            let iter = Arc::clone(&lk_iter);
            let thread = s.spawn(move |_| -> CIResult<()> {
                loop {
                    let linker = iter.lock().expect("failed to acquire lock").next();
                    if let Some(linker) = linker {
                        let crate_name = Arc::new(crate_name(&linker.bin_path));
                        info!("linking: {}", crate_name);
                        tx.send(IntegrationCx {
                            crate_name: Arc::clone(&crate_name),
                            state: State::Ld(false),
                        })?;
                        let object_files = linker.args.iter_mut().filter(|e| e.contains(".o"));
                        for file in object_files {
                            // find the object file contains the symbol for memory allocator
                            let output = ProcessBuilder::new(nm)
                                .arg("-jU")
                                .arg(&file)
                                .exec_with_output()?;
                            let stdout = String::from_utf8(output.stdout)?;
                            if stdout.contains("__rust_alloc") {
                                debug!("found allocator shim: {}", file);
                            } else {
                                *file = util::path_to_string(util::append_suffix(&file, "ci"));
                            }
                        }
                        let deps_rlib_files = linker
                            .args
                            .iter()
                            .filter(|e| e.contains("deps") && e.contains(".rlib"));
                        for file in deps_rlib_files {
                            debug!("replacing object file for rlib: {}", file);
                            // list all object files inside rlib
                            let output = ProcessBuilder::new(ar)
                                .arg("-t")
                                .arg(&file)
                                .exec_with_output()?;
                            let stdout = String::from_utf8(output.stdout)?;
                            if let Some(rcgu_obj_file_name) = stdout
                                .lines()
                                .find(|e| e.contains("rcgu") && !e.contains("-ci"))
                            {
                                debug!("found obj file: {}", rcgu_obj_file_name);
                                let rcgu_obj_file = deps_path.join(rcgu_obj_file_name);
                                let rcgu_obj_ci_file = util::append_suffix(&rcgu_obj_file, "ci");

                                // replace *.o with *-ci.o
                                ProcessBuilder::new(ar)
                                    .arg("-rb")
                                    .arg(&rcgu_obj_file)
                                    .arg(&file)
                                    .arg(&rcgu_obj_ci_file)
                                    .exec_with_output()?;

                                // delete old *.o
                                ProcessBuilder::new(ar)
                                    .arg("-d")
                                    .arg(&file)
                                    .arg(&rcgu_obj_file)
                                    .exec_with_output()?;
                            }
                        }

                        // execute the linker
                        debug!("linker: {:#?}", linker);
                        let mut builder = ProcessBuilder::new(&linker.program);
                        builder.args(&linker.args);
                        let output = builder.exec_with_output();
                        debug!("linker output: {:?}", output);
                        handle_output(output, &linker.bin_path, &tx, opts.debug_ci)?;
                        tx.send(IntegrationCx {
                            crate_name: Arc::clone(&crate_name),
                            state: State::Ld(true),
                        })?;
                    } else {
                        break;
                    }
                }
                Ok(())
            });
            threads.push(thread);
        }
        for thread in threads {
            thread
                .join()
                .expect("linker thread panicked")
                .context("linker failed")?;
        }

        drop(tx);

        pb_thread.join().expect("progress bar panicked");

        Ok(())
    })
    .expect("thread panicked")?;

    // copy CI-integrated binary file to the parent directory
    let binary_deps_files =
        util::scan_path(&deps_path, |p| p.executable() && p.is_file()).unwrap_or_default();
    let binary_examples_files =
        util::scan_path(&examples_path, |p| p.executable() && p.is_file()).unwrap_or_default();
    for file in binary_deps_files.iter().chain(binary_examples_files.iter()) {
        let file_name = crate_name(&file).replace("_", "-");
        let parent = file.parent().context("failed to get parent dir")?;
        let path = parent.with_file_name(file_name);
        let path = util::append_suffix(&path, "ci");
        paths::copy(file, path)?;
    }

    println!(
        "{:>12} integrated {} target(s) in {}",
        "Finished".green().bold(),
        linkers.len(),
        util::human_duration(time.elapsed())
    );

    Ok(())
}

/// Handle output from the process and validate output file.
fn handle_output<P: AsRef<Path>>(
    output: CIResult<Output>,
    output_file: P,
    tx: &mpsc::Sender<IntegrationCx>,
    debug: bool,
) -> CIResult<()> {
    let output_file = output_file.as_ref();
    let crate_name = Arc::new(crate_name(&output_file));
    match output {
        Ok(output) => {
            if !output_file.is_file() {
                // output file does not exist
                let stderr = String::from_utf8(output.stderr)?;
                let msg = "Process returned success but output file does not exist".to_string();

                tx.send(IntegrationCx {
                    crate_name: Arc::clone(&crate_name),
                    state: State::Error(msg),
                })?;

                bail!(
                    "process returned success but output file does not exist\n\
                    expected file: {}\n\
                    --- stderr\n{}",
                    output_file.display(),
                    stderr
                );
            }
            Ok(())
        }
        Err(err) => {
            let proc_err = err
                .downcast_ref::<ProcessError>()
                .context("process was not executed by `exec_with_output`")?;
            let mut out = proc_err.desc.lines().take(2).collect::<Vec<_>>();
            // take last few output lines to not polluting the terminal
            let mut desc = proc_err.desc.lines().rev().take(10).collect::<Vec<_>>();
            desc.reverse();
            out.push("(truncated)");
            out.append(&mut desc);
            let msg = if debug {
                let desc = &proc_err.desc;

                // set log file name
                let digest = md5::compute(desc.as_bytes());
                let date = chrono::Local::now().format("%y%m%dT%H%M%S").to_string();
                let mut path = util::config_path()?;
                path.push(format!("CI-{}-{:x}.log", date, digest));

                // log the entire output
                paths::write(&path, desc)?;

                format!(
                    "Consider filing an issue report on \
                    \"https://github.com/bitslab/CompilerInterrupts\" \
                    with the LLVM IR file and log attached. \
                    Path to the log: {}",
                    path.display(),
                )
            } else {
                "Run `cargo-build-ci` with `--debug-ci` to enable full logging".to_string()
            };

            tx.send(IntegrationCx {
                crate_name: Arc::clone(&crate_name),
                state: State::Error(msg),
            })?;

            bail!(out.join("\n"));
        }
    }
}

/// Run `cargo build` and return a vector contains linker command.
fn cargo_build(opts: &BuildOpts) -> CIResult<Vec<String>> {
    info!("running cargo build");

    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("build");

    if let Some(example) = &opts.example {
        cmd.arg("--example");
        cmd.arg(example);
    }

    // release mode
    if opts.release {
        cmd.arg("--release");
    }

    // target
    if let Some(target) = &opts.target {
        cmd.arg("--target");
        cmd.arg(target);
    }

    // color output
    cmd.env("CARGO_TERM_COLOR", "always");

    // print the internal linker invocation
    cmd.env("RUSTC_LOG", "rustc_codegen_ssa::back::link=info");

    // NOTE: cargo uses RUSTFLAGS first, hence overriding flags in config.toml
    // should find an alternative way to respect end-user's rustc flags
    // https://doc.rust-lang.org/cargo/reference/config.html#buildrustflags
    // moreover, adding external flags will trigger full re-compilation
    // when end-user executes normal `cargo build`

    // `--emit=llvm-ir` to emit LLVM IR bitcode
    // `-C save-temps` to save temporary files during the compilation
    // `-C passes` to pass extra LLVM passes to the compilation
    // https://doc.rust-lang.org/rustc/codegen-options/index.html

    // for some reason `env` does not escape quote in string literal...
    let rustflags = [
        "--emit=llvm-ir",
        "-Csave-temps",
        "-Cpasses=postdomtree",
        "-Cpasses=mem2reg",
        "-Cpasses=indvars",
        "-Cpasses=loop-simplify",
        "-Cpasses=branch-prob",
        "-Cpasses=scalar-evolution",
    ];
    cmd.env("RUSTFLAGS", rustflags.join(" "));

    let mut link_info = Vec::new();
    cmd.exec_with_streaming(
        &mut |out| {
            println!("{}", out);
            Ok(())
        },
        &mut |err| {
            if err.contains("INFO rustc_codegen_ssa::back::link") {
                link_info.push(err.to_string());
            } else if !err.is_empty() {
                eprintln!("{}", err);
            }
            Ok(())
        },
        false,
    )
    .context("Failed to execute `cargo build`")?;

    Ok(link_info)
}

/// Run `cargo metadata`.
fn cargo_metadata() -> CIResult<Metadata> {
    info!("running cargo metadata");
    let mut cmd = MetadataCommand::new();
    cmd.no_deps();
    let metadata = cmd.exec().context("Failed to execute `cargo metadata`")?;
    Ok(metadata)
}

/// Get the binary name from path.
fn crate_name<P: AsRef<Path>>(path: P) -> String {
    util::file_stem(path)
        .split('.')
        .next()
        .expect("invalid crate name, expected '.'")
        .split('-')
        .next()
        .expect("invalid crate name, expected '-'")
        .to_string()
}
