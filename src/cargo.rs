//! Cargo wrapper.

use std::path::PathBuf;

use anyhow::{bail, Context};
use cargo_util::ProcessBuilder;
use serde::{Deserialize, Serialize};
use tracing::{debug, info};

use crate::paths::PathExt;
use crate::CIResult;

/// Subset of information about the `cargo-build` invocation.
#[derive(Default, Debug)]
pub struct Cargo {
    /// Arguments.
    pub args: Vec<String>,
    /// Linkers.
    pub linkers: Vec<Linker>,
    /// Target directory.
    pub target_dir: PathBuf,
}

impl Cargo {
    /// Initialize Cargo with given arguments.
    pub fn with_args(args: Vec<String>) -> Self {
        Self {
            args,
            ..Default::default()
        }
    }

    /// Runs `cargo-build`.
    pub fn build(&mut self) -> CIResult<()> {
        info!("running cargo build");

        let mut cmd = ProcessBuilder::new("cargo");
        cmd.arg("build");
        cmd.args(&self.args);

        // color output
        cmd.env("CARGO_TERM_COLOR", "always");

        // print the internal linker invocation
        cmd.env("RUSTC_LOG", "rustc_codegen_ssa::back::link=info");

        // print the output files
        cmd.env(
            "CARGO_LOG",
            "cargo::core::compiler::context::compilation_files=debug",
        );

        // `--emit=llvm-ir` to emit LLVM IR bitcode
        // `-C save-temps` to save temporary files during the compilation
        // https://doc.rust-lang.org/rustc/codegen-options/index.html

        let rustflags = ["--emit=llvm-ir", "-Csave-temps"];
        cmd.env("RUSTFLAGS", rustflags.join(" "));

        debug!(?cmd);

        let mut link_info = Vec::new();
        let mut compilation_files = Vec::new();
        cmd.exec_with_streaming(
            &mut |out| {
                println!("{}", out);
                Ok(())
            },
            &mut |err| {
                if err.contains("rustc_codegen_ssa::back::link") {
                    link_info.push(err.to_string());
                } else if err.contains("cargo::core::compiler::context::compilation_files") {
                    compilation_files.push(err.to_string());
                } else if !err.is_empty() {
                    eprintln!("{}", err);
                }
                Ok(())
            },
            false,
        )
        .context("failed to execute `cargo build`")?;

        debug!(?link_info);
        debug!(?compilation_files);

        self.linkers = parse_linkers(link_info)?;
        let output_files = parse_output_files(compilation_files)?;
        self.target_dir = target_dir(output_files)?;

        Ok(())
    }
}

/// Linker invocation.
#[derive(Debug)]
pub struct Linker {
    /// Linker program name.
    pub program: String,
    /// Arguments for the linker.
    pub args: LinkerArgs,
}

/// Arguments of a linker invocation.
#[derive(Debug)]
pub struct LinkerArgs {
    /// List of input files.
    pub input_files: Vec<String>,
    /// Path to the output file.
    pub output_file: String,
    /// List of rlib files.
    pub rlib_files: Vec<String>,
    /// List of directories for library search.
    pub library_dirs: Vec<String>,
    /// Other flags.
    pub flags: Vec<String>,
}

impl LinkerArgs {
    /// Build a complete linker arguments.
    pub fn build(self) -> Vec<String> {
        let mut all = Vec::new();
        all.extend(self.input_files);
        all.push("-o".to_string());
        all.push(self.output_file);
        all.extend(self.rlib_files);
        for dir in self.library_dirs {
            all.push("-L".to_string());
            all.push(dir);
        }
        all.extend(self.flags);
        all
    }
}

/// Information of a file flavor.
#[derive(Serialize, Deserialize, Clone, Copy, Debug)]
pub enum FileFlavor {
    /// Not a special file type.
    Normal,
    /// Like `Normal`, but not directly executable.
    /// For example, a `.wasm` file paired with the "normal" `.js` file.
    Auxiliary,
    /// Something you can link against (e.g., a library).
    Linkable,
    /// An `.rmeta` Rust metadata file.
    Rmeta,
    /// Piece of external debug information (e.g., `.dSYM`/`.pdb` file).
    DebugInfo,
}

/// Information of an output file.
#[derive(Serialize, Deserialize, Debug)]
pub struct OutputFile {
    /// Absolute path to the file that will be produced by the build process.
    pub path: PathBuf,
    /// If it should be linked into `target`, and what it should be called
    /// (e.g., without metadata).
    pub hardlink: Option<PathBuf>,
    /// If `--out-dir` is specified, the absolute path to the exported file.
    pub export_path: Option<PathBuf>,
    /// Type of the file (library / debug symbol / else).
    pub flavor: FileFlavor,
}

/// Parse the linker invocation from raw build output.
fn parse_linkers(link_info: Vec<String>) -> CIResult<Vec<Linker>> {
    debug!("parsing linkers");
    let mut linkers = Vec::new();
    for line in link_info {
        if !line.contains("libcompiler_builtins") {
            // ignore as this is highly not a linker invocation
            continue;
        }

        let line = line.replace('\"', "");
        let mut linker = line
            .split_ascii_whitespace()
            .skip(2) // skip "INFO", "rustc_codegen_ssa::back::link"
            .map(str::to_string);
        let program = linker.next().context("missing linker program name")?;

        let mut input_files = Vec::new();
        let mut output_file = String::new();
        let mut rlib_files = Vec::new();
        let mut library_dirs = Vec::new();
        let mut flags = Vec::new();

        while let Some(arg) = linker.next() {
            if arg.contains("-o") {
                output_file = linker.next().context("missing output file")?;
            } else if arg.contains("-L") {
                library_dirs.push(linker.next().context("missing library dir")?);
            } else {
                let path = PathBuf::from(&arg);
                if path.is_file() {
                    if path.extension().unwrap_or_default() == "rlib" {
                        rlib_files.push(arg);
                    } else {
                        input_files.push(arg);
                    }
                } else {
                    flags.push(arg);
                }
            }
        }

        linkers.push(Linker {
            program,
            args: LinkerArgs {
                input_files,
                output_file,
                rlib_files,
                library_dirs,
                flags,
            },
        });
    }
    debug!(?linkers);
    Ok(linkers)
}

/// Parse the output files from raw build output.
fn parse_output_files(compilation_files: Vec<String>) -> CIResult<Vec<OutputFile>> {
    debug!("parsing output files");
    let mut output_files = Vec::new();
    for line in compilation_files {
        let line = line.split_inclusive(']').collect::<Vec<_>>()[1]
            .replace(" Target filenames: ", "")
            .replace('{', "(")
            .replace('}', ")");
        output_files.append(&mut ron::from_str(&line)?);
    }
    debug!(?output_files);
    Ok(output_files)
}

/// Gets the target directory of the workspace.
fn target_dir(output_files: Vec<OutputFile>) -> CIResult<PathBuf> {
    debug!("parsing target directory");

    let mut target_dirs = Vec::new();
    let mut targets = Vec::new();
    let mut modes = Vec::new();
    for file in output_files {
        if let Some(hardlink) = &file.hardlink {
            debug!(?hardlink);

            if hardlink.file_name()?.contains("build-script-build") {
                continue;
            }
            let mut dir = hardlink.parent()?;
            if dir.file_name()? == "examples" {
                dir = dir.parent()?;
            }
            modes.push(dir.file_name()?);
            dir = dir.parent()?;
            if dir.file_name()? == "target" {
                target_dirs.push(dir);
            } else {
                targets.push(dir.file_name()?);
                target_dirs.push(dir.parent()?);
            }
        }
    }

    let target_dir = &target_dirs[0];
    let mode = &modes[0];
    let target = if !targets.is_empty() {
        Some(&targets[0])
    } else {
        None
    };
    debug!(?target_dir);
    debug!(?mode);
    debug!(?target);

    // sanity check
    for dir in &target_dirs {
        if dir.file_name()? != target_dir.file_name()? {
            bail!("failed to parse target directory");
        }
    }

    if !modes.iter().all(|e| e == mode) {
        bail!("failed to parse target directory");
    }

    if let Some(target) = target {
        if !targets.iter().all(|e| e == target) {
            bail!("failed to parse target directory");
        }

        Ok(target_dir.join(target).join(mode))
    } else {
        Ok(target_dir.join(mode))
    }
}

/// Gets the root directory of the workspace.
pub fn locate_project() -> CIResult<PathBuf> {
    let mut cmd = ProcessBuilder::new("cargo");
    cmd.arg("locate-project");
    cmd.arg("--message-format=plain");
    let output = cmd.exec_with_output()?;
    let stdout = String::from_utf8(output.stdout)?;
    stdout.parent()
}
