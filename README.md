KOMPARE AMP Eval
=============================

Please see the README for the `klee-compare` branch first.

This branch is additionally modified to support evaluating the AMP Challenge Programs by stubbing out certain system functions in `POSIX-Compare`.

To run the AMP Challenges using KOMPARE, do the following:

1. Start with the repo for the Ironpatch tool, which will include the driver program `superglue.c`. This driver program will send the symbolic CAN frame to the program.

1. Replace (any instances of) the file `glue.c` with the `glue.c` included in this repo under `/amp/glue.c`. This modified `glue.c` removes some of the function stubs which have been moved to the `POSIX-Compare` runtime.

1. In `superglue.c`, comment out the call to `vuln_main()` as well as the `printf`s before it which print `calling vuln main` (these must be commented out or the outputs will be differing)

1. `make` and then `mv linked.bc sg_vuln.bc`

1. Uncomment the call to `vuln_main()` and then comment the call to `patched_main()`. Once again, `make` and then `mv linked.bc sg_patched.bc`

1. Make `KOMPARE` and then invoke `./build/bin/klee-compare sg_patched.bc sg_vuln.bc`

Note that by default this will compare the outputs exactly. To specify a different output comparison, add the function to `tools/klee-compare/main.c`. An output comparison for Challenge 5 is provided in a function called `challenge_5_comparison(...)` for instance. This is needed if we expect the outputs to differ along all paths for any reason.

KLEE Symbolic Virtual Machine
=============================

[![Build Status](https://github.com/klee/klee/workflows/CI/badge.svg)](https://github.com/klee/klee/actions?query=workflow%3ACI)
[![Build Status](https://api.cirrus-ci.com/github/klee/klee.svg)](https://cirrus-ci.com/github/klee/klee)
[![Coverage](https://codecov.io/gh/klee/klee/branch/master/graph/badge.svg)](https://codecov.io/gh/klee/klee)

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
