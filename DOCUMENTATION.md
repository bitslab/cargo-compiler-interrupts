# Documentation

[![docs.rs](https://docs.rs/cargo-compiler-interrupts/badge.svg)](https://docs.rs/cargo-compiler-interrupts)

## Objective

Since [Compiler Interrupts](https://dl.acm.org/doi/10.1145/3453483.3454107) is an LLVM pass, we want to extend `cargo` to support applying a third-party LLVM pass to the binary during the compilation process seamlessly. `cargo-compiler-interrupts` made specifically to integrate the Compiler Interrupts in just one command.

## Integration

* If the library hasn't been installed yet, run `cargo-lib-ci install` to install the library first. Make sure you have Rust and LLVM toolchain installed.
* Register the Compiler Interrupts handler in your program. Compiler Interrupts APIs are provided through the [`compiler-interrupts`](https://github.com/bitslab/compiler-interrupts-rs) package. You can check out the [`ci-demo`](https://github.com/bitslab/compiler-interrupts-rs/tree/master/ci-demo) in the `compiler-interrupts` package for more detailed usages.

``` rust
fn interrupt_handler(ic: i64) {
    println!("Compiler interrupt called with instruction count: {}", ic);
}

fn main() {
    unsafe {
        compiler_interrupts::register(1000, 1000, interrupt_handler);
    }

    // your code
    for _ in 0..100 {
        println!("hello world!");
    }
}
```

* Run `cargo-build-ci` to start the compilation and integration processes.
* Run `cargo-run-ci` to run the CI-integrated binary.

## Options

`cargo-compiler-interrupts` provides three binaries:

```
Compile and integrate the Compiler Interrupts to a package

Usage: cargo-build-ci [OPTIONS] [-- <CARGO_BUILD_ARGS>...]

Arguments:
  [CARGO_BUILD_ARGS]...  Arguments for `cargo` invocation

Options:
      --skip <CRATES>  Crates to skip the integration (space-delimited)
      --debug          Enable debugging mode for Compiler Interrupts library
      --log <LEVEL>    Log level [default: warn] [possible values: trace, debug, info, warn, error]
  -h, --help           Print help information
  -V, --version        Print version information
```

```
Run a Compiler Interrupts-integrated binary

Usage: cargo-run-ci [OPTIONS] [-- <ARGS>...] [-- <CARGO_RUN_ARGS>...]

Arguments:
  [ARGS]...            Arguments for the binary
  [CARGO_RUN_ARGS]...  Arguments for `cargo` invocation

Options:
      --bin <NAME>   Name of the binary
      --log <LEVEL>  Log level [default: warn] [possible values: trace, debug, info, warn, error]
  -h, --help         Print help information
  -V, --version      Print version information
```

```
Manage the Compiler Interrupts library

Usage: cargo-lib-ci [OPTIONS] [COMMAND]

Commands:
  install    Install the Compiler Interrupts library
  uninstall  Uninstall the Compiler Interrupts library
  update     Update the Compiler Interrupts library
  config     Configure the Compiler Interrupts library
  help       Print this message or the help of the given subcommand(s)

Options:
      --log <LEVEL>  Log level [default: warn] [possible values: trace, debug, info, warn, error]
  -h, --help         Print help information
  -V, --version      Print version information
```

## How does it work?

1. `cargo build-ci` will invoke `cargo build` with `RUSTC_LOG=rustc_codegen_ssa::back::link=info` to output internal linker invocations. It also adds a bunch of extra flags to all `rustc` invocations. Extra flags are:
    * `--emit=llvm-ir` — emit LLVM IR bitcode in the LLVM assembly language format.
    * `-C save-temps=y` — all temporary output files during the compilation.
    * `-C passes=...` — LLVM optimization passes for optimizing CI overhead.
2. After `cargo build` completed, we should have these:
    * Output from `cargo build` contains internal linker commands that are generated by `rustc` for every library and binary.
    * Object `*.o` files and IR bitcode in the LLVM assembly language `*.ll` files in the `$CARGO_TARGET_DIR/<build_mode>/deps` directory. Moreover, each file should have a corresponding intermediate version that contains `rcgu` (rust codegen unit) in their name.
    * Rust static library with extra metadata `*.rlib` files. These files are generated if the project has extra modules and dependencies.
3. Run `opt` on all intermediate IR bitcode `*.ll` files to integrate the Compiler Interrupts. All CI-integrated files have the suffix `_ci` in their name.
4. Run `llc` to convert CI-integrated IR bitcode `*.ll` files to object `*.o` files.
5. Parse the output from `cargo build` to get the linker command for the binary. The linker command consists of a variety of arguments relating to the output file, linking rust-std/system libraries, and specifying `*.rlib` dependencies for the binary.
6. Find the allocator shim, which is a special intermediate object file that contains the symbols for the Rust memory allocator. `rustc` automatically generates the allocator shim behind the scene.
7. Replace the object file in the `*.rlib` with the CI-integrated one.
8. Execute the linker command again to output the final CI-integrated binary.
9. All CI-integrated artifacts are output to `$CARGO_TARGET_DIR/<build_mode>/deps-ci`. CI-integrated binary has their name appended with `-ci` suffix.

## Limitations

* `cargo build` outputs the artifacts for us to replace the object files with the CI-integrated one, then we invoke the linker one more time to output the new CI-integrated binary. Therefore, we have to compile the binary twice.
* Assuming the Compiler Interrupts does not depend on built-in `opt` optimizations, we can make some changes to `rustc` so that it can load and register a third-party LLVM pass during the compilation, hence eliminating the `opt` stage and linking after that, making the process done in one go. As a matter of fact, `clang` supports loading and registering a third-party LLVM pass by running `clang -Xclang -load -Xclang mypass.so`, albeit the usage is more complicated than `opt` and does not support built-in passes from `opt`. Currently, there is a [request](https://github.com/rust-lang/compiler-team/issues/419) to the Rust compiler team to enable this functionality.
* Since we have to depend on the build output, `cargo-compiler-interrupts` might not be robust against major changes.
* Compiler Interrupts integration is not fast on huge IR bitcode from crates such as `clap`, `derive`, `proc`, `regex`, `serde`, `syn`, `toml`,... We roughly estimate the integration process takes about an hour for 500,000 lines of IR bitcode on an x86-64 quad-core machine.
