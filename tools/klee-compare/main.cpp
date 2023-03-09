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
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
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
    TargetFile(cl::desc("<patched (target) bytecode>"), cl::Positional, cl::Required);

    cl::opt<string>
    CompareFile(cl::desc("<original (comparison) bytecode>"), cl::Positional, cl::Required);

    cl::list<string>
    InputArgv(cl::ConsumeAfter,
              cl::desc("<program arguments>..."));
}

// help function to create output directory as "klee-compare-out-<i>"
// copied from klee/main.c
string create_output_dir() {
    string outdir = "";
    
    int i = 0;
    SmallString<128> directory(TargetFile);
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
        if (std::filesystem::create_directory(d.c_str())) {
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

// go through the streams and return true if the contents of the files are different
bool files_differ(std::ifstream &file_a, std::ifstream &file_b) {
    string line_a, line_b;
    while(std::getline(file_a, line_a)) {
        if(!std::getline(file_b, line_b)) {
            return true;
        }
        if (line_a != line_b) {
            return true;
        }
    }
    // we've finished reading file_a, check if there's anything else to read in file_b
    if(std::getline(file_b, line_b)) {
        return true;
    }
    return false;
}

// helper function to run an instance of KLEE using a ktest
// TODO: print diff of the program outputs in the results file or so
void run_klee_instance(string klee_command, string outdir, string ktest, string target) {
    // set this for the POSIX-Compare Runtime
    // there is no std version of setenv? using stdlib.h version
    // TODO: this breaks things, not sure why. we'll just used a hardcoded /tmp path
    // setenv("KLEE_OUTPUT_PATH", outdir.c_str(), 1);

    string com = klee_command + " --posix-compare --output-dir " + outdir;
    com += " --replay-ktest-file " + ktest + " " + target;
    // TODO: make this a command line argument
    // com += " &> /dev/null"; // don't wanna print this output
    
    if (DEBUG_PRINTS) std::cout << "Running command " << com << std::endl;

    FILE *fd = popen(com.c_str(), "w");

    // wait for the instance of KLEE to terminate
    pclose(fd);

    // move the output file so we can inspect it before the next run
    // the /tmp filepath is hardcoded in POSIX-Compare/fd.c
    // TODO: extra file moving is not really necessary, we might as well load this into a buffer now
    if(std::filesystem::exists("/tmp/klee_compare_dump.txt")) {
        std::filesystem::rename("/tmp/klee_compare_dump.txt", outdir + "/compare_dump.txt");
    } else {
        // if no such file existed, we'll just create an empty file in the output dir
        std::ofstream f(outdir + "/compare_dump.txt");
        f.close();
    }
}

void watch_klee_output(string watchdir, std::queue<string> *ktests) {
    // watch the output directory for KLEE
    int notif, wdir;
    char buffer[EVENT_BUF_LEN];

    notif = inotify_init();
    wdir = inotify_add_watch(notif, watchdir.c_str(), IN_CREATE);

    // the read call will block until a file is written by KLEE
    // so we can loop forever on this thread and kill it when KLEE is done
    while (true) {
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
                        ktests->push(filename);
                    }
                }
            }   
            i += EVENT_SIZE + event->len;
        }
    }

    inotify_rm_watch(notif, wdir);
    close(notif);
}

// this function is run in a separate thread than the main thread which looks for ktest files
// this does the actual comparison between the two versions of the programs
// it stops when "done" is set to true by the caller thread (or something)
// this thread also handles writing the results
void compare(bool *done, string klee_command, string outdir, std::queue<string> *ktests) {
    int paths = 0;
    int differences = 0;
    std::ofstream resout(outdir + "/results.txt");

    while(!(*done)) {
        while (!ktests->empty()) {
            // create the command which we'll use to replay the test with KLEE 
            string test = ktests->front();
            ktests->pop();

            // set output dir as patched or program out
            string patched_outdir = outdir + "/klee-patched-out";
            string original_outdir = outdir + "/klee-original-out";

            // run both instances of KLEE for comparison
            run_klee_instance(klee_command, patched_outdir, outdir + "/klee-out/" + test, TargetFile);
            run_klee_instance(klee_command, original_outdir, outdir + "/klee-out/" + test, CompareFile);

            // get the results from outdir/write_dump.txt and compare them
            std::ifstream patched_dump(patched_outdir + "/compare_dump.txt");
            std::ifstream original_dump(original_outdir + "/compare_dump.txt");

            bool differs = files_differ(patched_dump, original_dump);

            patched_dump.close();
            original_dump.close();

            string res = "Outputs" + string(differs ? " DIFFER " : " MATCH " ) + "on test: " + test;

            if (DEBUG_PRINTS) std::cout << res << std::endl;
            resout << res << std::endl;

            if (differs) differences += 1;

            // delete output dirs before next run
            std::filesystem::remove_all(patched_outdir);
            std::filesystem::remove_all(original_outdir);

            paths += 1;
        }
        // ktests must be empty, let KLEE run for a bit more before we try again
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // print summary of comparison results
    resout << "\nPaths compared: " << paths << std::endl;
    resout << "Paths differing: " << differences << std::endl;
    resout.close();
}

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(argc, argv, "");

    if (TargetFile == "" || CompareFile == "") {
        std::cout << "Error: missing input file(s)" << std::endl;
        return 1;
    }

    // get the KLEE path as an environemnt variable
    const char *klee_path = std::getenv("KLEE_PATH");
    assert(klee_path && "Please set KLEE_PATH env var as the directory containing KLEE executable");

    // create the output directory
    string outdir = create_output_dir();
    string outdir_klee = outdir + "/klee-out";

    // we'll create the klee output directory before klee does so that way we can
    // watch it with inotify without missing any events
    std::filesystem::create_directory(outdir_klee.c_str());

    // create the command to run KLEE
    // hardcoding uclibc and posix-runtime args for now
    // TODO: these arguments should be set as options for klee-compare and passed through to klee
    string klee_command_prefix = string(klee_path) + "/klee --libc=uclibc --posix-runtime";
    string klee_command = klee_command_prefix + " --output-dir " + outdir_klee;

    // use patch-directed symbolic execution (TODO: make this a program option)
    klee_command += " --search patch-priority --compare-bitcode " + CompareFile;

    for (unsigned i = 0; i < InputArgv.size() + 1; i++) {
        string &arg = (i==0 ? TargetFile : InputArgv[i-1]);
        klee_command += " " + arg;
    }

    // redirect output from KLEE
    klee_command += " &> " + outdir + "/klee_out.txt";

    // spawn an instance of klee which will explore the program
    FILE *kleefd = popen(klee_command.c_str(), "w");
    assert(kleefd != nullptr && "Could not start KLEE instance");

    bool done = false;
    std::queue<string> ktests;

    // launch the compare function in a thread, which recieves the test files
    // in the queue once we know they exist
    std::thread comparison_thread(compare, &done, klee_command_prefix, outdir, &ktests);
    
    // thread which collects tests output from KLEE and queues them
    std::thread output_watch_thread(watch_klee_output, outdir_klee, &ktests);
    
    // sleep for a bit and then join the other thread once it's done
    pclose(kleefd);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    done = true; // stops loop in comparison_thread 
    comparison_thread.join();
    // TODO: is there a nicer way to this? make complains when it gets the interrupt
    pthread_kill(output_watch_thread.native_handle(), SIGINT);
}
