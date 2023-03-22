#ifndef _KLEE_PATCH_EXPLORER_H
#define _KLEE_PATCH_EXPLORER_H

#include "Executor.h"
#include "llvm/IR/Module.h"

namespace klee {

// This class takes the LLVM diff file and generates the priorities to direct execution  
// towards the patch code. The pass which explores the code is executed upon initialization
// and depends on the command line opt to get the llvm-diff output file. It stores the 
// resulting instruction-priority map in a public variable so that it can be accessed by the 
// patch explorer during runtime.
class PatchExplorer {
public:

    // pointer to the main module
    llvm::Module *mainModule;

    // the module we want to compare against
    llvm::Module *cmpModule;

    // enable pruning
    bool pruning = false;

    PatchExplorer(Executor *executor);

    uint64_t getPriority(llvm::Instruction *inst);

    // check if an instruction is patch code
    // does this by checking if parent BB is in patchInstructions set
    bool isPatchCode(llvm::Instruction *inst);

    // print all non-zero priorities to llvm::errs() for debugging
    void dumpPriorities();

    // print out the entire program for debugging purposes
    void dumpProgram();

private:

    // priorities, should be access through get_priority function
    std::unordered_map<llvm::Instruction *, uint64_t> priorities;

    // these instructions are all considered "patch code"
    // after executig any of these, we want to explore the remaining program entirely
    std::unordered_set<llvm::BasicBlock *> patchInstructions;
};

}

#endif /* _KLEE_PATCH_EXPLORER_H */