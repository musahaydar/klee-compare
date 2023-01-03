// klee-compare is a wrapper for patch comparison which invokes instances of KLEE

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>

using namespace llvm;
using std::string;

#define DEBUG_PRINTS 1

// constants for inotify
#define EVENT_SIZE (sizeof (struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

namespace {
    cl::opt<string>
    InputFile(cl::desc("<input bytecode>"), cl::Positional, cl::init("-"));

    cl::list<string>
    InputArgv(cl::ConsumeAfter,
              cl::desc("<program arguments>..."));
}

// help function to create output directory as "klee-compare-out-<i>"
// copied from klee/main.c
string create_output_dir() {
    string outdir = "";
    
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

// this function is run in a separate thread than the main thread which looks for ktest files
// this does the actual comparison between the two versions of the programs
// it stops when "done" is set to true by the caller thread or something :shrug:
void compare(bool *done, string klee_command, string klee_out_dir, std::queue<string> *ktests) {
    while(!*done) {
        while (!ktests->empty()) {
            // create the command which we'll use to replay the test with KLEE 
            string test = ktests->front();
            ktests->pop();

            string com = klee_command + " --replay-ktest-file " + klee_out_dir + "/" + test + " " + InputFile;
            // TODO: set output dir as patched or program out
            FILE *fp = popen(com.c_str(), "w");

            if (DEBUG_PRINTS) std::cout << "Running command " << com << std::endl;

            // wait for the instance of KLEE to terminate
            pclose(fp);

            // TODO: run the other version of the program with the same generated ktest file
            // TOOD: compare the results, log that a path has been explored and if a difference was found
            // TODO: delete dirs "patched-out" and "program-out" before next run
        }
        // ktests must be empty, let KLEE run for a bit more before we try again
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(argc, argv, "");

    // get the KLEE path as an environemnt variable
    const char *klee_path = std::getenv("KLEE_PATH");
    assert(klee_path && "Please set KLEE_PATH env var as the directory containing KLEE executable");

    // create the output directory
    string outdir = create_output_dir();
    string outdir_klee = outdir + "/klee-out";

    // we'll create the klee output directory before klee does so that way we can
    // watch it with inotify without missing any events
    mkdir(outdir_klee.c_str(), 0775);

    // create the command to run KLEE
    // hardcoding uclibc and posix-runtime args for now
    // TODO: these arguments should be set as options for klee-compare and passed through to klee
    string klee_command_prefix = string(klee_path) + "/klee --libc=uclibc --posix-runtime";
    string klee_command = klee_command_prefix + " --output-dir " + outdir_klee;
    for (unsigned i = 0; i < InputArgv.size() + 1; i++) {
        string &arg = (i==0 ? InputFile : InputArgv[i-1]);
        klee_command += " " + arg;
    }

    // redirect output from KLEE
    klee_command += " &> " + outdir + "/klee_out.txt";

    // spawn an instance of klee which will explore the program
    FILE *kleefp = popen(klee_command.c_str(), "w");
    assert(kleefp != nullptr && "Could not start KLEE instance");

    // watch the output directory for KLEE
    int notif, wdir;
    char buffer[EVENT_BUF_LEN];

    notif = inotify_init();
    wdir = inotify_add_watch(notif, outdir_klee.c_str(), IN_CREATE);

    bool done = false;
    std::queue<string> ktests;

    // launch the compare function in a thread, which recieves the test files
    // in the queue once we know they exist
    std::thread comparison_thread(compare, &done, klee_command_prefix, outdir_klee, &ktests);

    while (!done) {
        // the read call will block until a file is written by KLEE
        int length = read(notif, buffer, EVENT_BUF_LEN);
        assert(length != 0 && "Error reading from inotify");

        // Read and process the inotify event
        int i = 0;
        while (i < length) {     
            inotify_event *event = (inotify_event *) &buffer[i];     
            if (event->len) {
                if (event->mask & IN_CREATE && !(event->mask & IN_ISDIR)) {
                    string filename(event->name);
                    // test files are all named like "test000006.ktest"
                    // we can just check for the ktest where we expect it + string length
                    if (filename.length() == 16 && filename.substr(10, 6) == ".ktest") {
                        // we have a test file to compare!
                        if (DEBUG_PRINTS) printf("New test file %s found.\n", event->name);
                        ktests.push(filename);
                    }
                }
            }   
            i += EVENT_SIZE + event->len;
        }
    }

    inotify_rm_watch(notif, wdir);
    close(notif);

    // wait for the instance of KLEE to terminate
    pclose(kleefp);

    // TODO: throw comparison instances of klee's output in /dev/null
    // TODO: print paths explored, differences found
}
