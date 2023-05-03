KOMPARE
=============================

`KOMPARE` is an extension for `KLEE` which uses symbolic exeuction for automated program comparison. After compiling `KLEE` as usual, run `KOMPARE` as follows:

```
./build/bin/klee-compare [options] <patched.bc> <original.bc>
```

- `<patched.bc>` is the version of the program (in LLVM bitcode format) with the patch applied. This version will be symbolically executed.
- `<original.bc>` is the unpached version of the program (in LLVM bitcode format).

In order to use the `klee-compare` driver, the `KLEE_PATH` environment variable needs to be set (run `export KLEE_PATH=path/to/klee/build/bin` with the path being the directory containing `klee` and `klee-compare`). If it's not set, `klee-compare` will complaion.

The options supported are:

- `--directed`: enabled patch-directed symbolic execution
- `--pruning`: (EXPERIMENTAL) enable path pruning under patch-directed symbolic execution

## Extending KOMPARE

- To exend `KOMPARE` to support comparing additional externally visible outputs than those included in this repo, the function wrapper should be added to `runtime/POSIX-compare/fd.c` with the string `kcmp_` prepended to the function name (i.e. `fwrite` becomes `kcmp_fwrite`). Then, the function name should be added to the list of functions to be renamed during linking in `tools/klee/main.c`.

## Other Modifications to KLEE:

Some of the modifications to `KLEE` can be used outside of the `KOMPARE` driver as follows:

- To use the modified POSIX runtime for comparison in `KLEE`, add the  `--posix-compare` after the `--posix-runtime`. The modified POSIX enviroment will output the data sent to certain system calls (such as `fwrite`, `fputs`, `printf`, etc. The full list can be found in `tools/klee/main.c`) to the file located in `/tmp/klee_compare_dump.txt`. When using `KOMPARE`, this file is collected and removed by the driver automatically.
- To use patch-directed symbolic execution in `KLEE`, add the following options: `--search patch-priority --compare-bitcode <original.bc>`

KLEE Symbolic Virtual Machine
=============================

`KLEE` is a symbolic virtual machine built on top of the LLVM compiler
infrastructure. Currently, there are two primary components:

  1. The core symbolic virtual machine engine; this is responsible for
     executing LLVM bitcode modules with support for symbolic
     values. This is comprised of the code in lib/.

  2. A POSIX/Linux emulation layer oriented towards supporting uClibc,
     with additional support for making parts of the operating system
     environment symbolic.

Additionally, there is a simple library for replaying computed inputs
on native code (for closed programs). There is also a more complicated
infrastructure for replaying the inputs generated for the POSIX/Linux
emulation layer, which handles running native programs in an
environment that matches a computed test input, including setting up
files, pipes, environment variables, and passing command line
arguments.

For further information, see the [webpage](http://klee.github.io/).
