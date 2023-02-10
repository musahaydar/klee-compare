#include "PatchExplorer.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <iostream>

namespace klee {

PatchExplorer::PatchExplorer(Executor *executor)
    : mainModule(executor->kmodule->module.get()),
      cmpModule(executor->cmpModule->module.get()) {
    
    for (llvm::Function &func : *mainModule) {
        // llvm::errs() << "Function: " << func.getName().str() << "\n";
        llvm::Function *cmpFunc = cmpModule->getFunction(func.getName());
        if (cmpFunc == nullptr) {
            llvm::errs() << "Function: " << func.getName().str() << " does NOT exist in original bitcode\n";
            continue;
        }

        for (llvm::BasicBlock &bb : func) {
            // this will print unnamed BBs with their number instead
            // llvm::errs() << "\tBasic Block: ";
            // bb.printAsOperand(llvm::errs(), false);
            // llvm::errs() << "\n";

            for (llvm::Instruction &inst : bb) {
                // llvm::errs() << "\t\tInstruction: " << inst << "\n";

            }
        }
    }
}

}