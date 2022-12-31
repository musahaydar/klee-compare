// klee-compare is a wrapper for patch comparison which invokes instances of KLEE

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <string>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

using namespace llvm;
using std::string;

namespace {
    cl::opt<string>
    InputFile(cl::desc("<input bytecode>"), cl::Positional, cl::init("-"));

    cl::list<string>
    InputArgv(cl::ConsumeAfter,
              cl::desc("<program arguments>..."));
}

string create_output_dir() {
    string outdir = "";
    // create output directory as "klee-compare-out-<i>"
    // copied from klee/main.c
    int i = 0;
    SmallString<128> directory(InputFile);
    sys::path::remove_filename(directory);
    if (auto ec = sys::fs::make_absolute(directory)) {
        std::cout << "Unable to determine absolute path of working dir" << std::endl;
    }

    for (; i <= INT_MAX; ++i) {
        SmallString<128> d(directory);
        llvm::sys::path::append(d, "klee-compare-out-");
        raw_svector_ostream ds(d);
        ds << i;
        // SmallString is always up-to-date, no need to flush. See Support/raw_ostream.h

        // create directory and try to link klee-last'
        // otherwise try again or exit on error
        if (mkdir(d.c_str(), 0775) == 0) {
            outdir = d.str().str(); // StringRef to string lol
            break;
        }
    }
    if (i == INT_MAX && outdir == "") {
        std::cout << "Error: cannot create output directory. index out of range" << std::endl;
    }
    
    std::cout << "Output directory is " << outdir << std::endl;
    return outdir;
}

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(argc, argv, "");

    // get the KLEE path as an environemnt variable
    const char *klee_path = std::getenv("KLEE_PATH");
    assert(klee_path && "Please set KLEE_PATH env var as the directory containing KLEE executable");

    // create the output directory
    string outdir = create_output_dir();

    // create the command to run KLEE
    // hardcoding uclibc and posix-runtime args for now
    // TODO: these arguments should be set as options for klee-compare and passed through to klee
    string klee_command = string(klee_path) + "/klee --libc=uclibc --posix-runtime";
    klee_command += " --output-dir " + outdir + "/klee-out";
    for (unsigned i = 0; i < InputArgv.size() + 1; i++) {
        string &arg = (i==0 ? InputFile : InputArgv[i-1]);
        klee_command += " " + arg;
    }

    // redirect output from KLEE
    klee_command += " &> " + outdir + "/klee_out.txt";

    // spawn an instance of klee which will explore the program
    FILE *kleefp = popen(klee_command.c_str(), "w");
    assert(kleefp != nullptr && "Could not start KLEE instance");

    // wait for the instance of KLEE to terminate
    pclose(kleefp);

    // TODO: throw comparison instances of klee's output in /dev/null
}