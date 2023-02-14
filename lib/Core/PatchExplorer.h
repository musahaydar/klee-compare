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

    PatchExplorer(Executor *executor);

    // print all non-zero priorities to llvm::errs() for debugging
    void dumpPriorities();

private:

    // priorities, should be access through get_priority function
    std::unordered_map<llvm::Instruction*, uint64_t> priorities;

};

}

#endif /* _KLEE_PATCH_EXPLORER_H */