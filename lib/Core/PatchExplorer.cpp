#include "PatchExplorer.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define DEBUG_PRINTS 0

// compare two instructions as strings, return true if they're equal
bool instructionsEqual(std::string inst1, std::string inst2) {
    std::stringstream inst1ss(inst1);
    std::stringstream inst2ss(inst2);
    std::string substr1 = "", substr2 = "";

    while(inst1ss >> substr1) {
        inst2ss >> substr2;
        // ignore metadata
        if (substr1[0] == '!' && substr2[0] == '!') {
            continue;
        }
        if (substr1 != substr2) {
            // ignore differing struct nums, not sure if this difference implies anything tho...
            if (substr1.find("struct") != std::string::npos && substr2.find("struct") != std::string::npos) {
                continue;
            }

            return false;
        }
    }
    return true;
}

// helper function from Agamotto
// https://github.com/efeslab/agamotto/blob/artifact-eval-osdi20/lib/Core/NvmAnalysisUtils.cpp
llvm::Instruction *getReturnLocation(llvm::CallBase *cb) {
    if (llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(cb)) {
        return ci->getNextNode();
    } else if (llvm::InvokeInst *ii = llvm::dyn_cast<llvm::InvokeInst>(cb)) {
        return ii->getNormalDest()->getFirstNonPHI();
    } 
    
    return nullptr;
}

llvm::Function *getCallInstFunction(llvm::CallBase *cb) {
    if (!cb) return nullptr;

    if (llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(cb)) {
        if (ci->isInlineAsm()) return nullptr;
    }

    llvm::Function *cfn = cb->getCalledFunction();
    if (!cfn) {
        cfn = llvm::dyn_cast<llvm::Function>(cb->getCalledOperand()->stripPointerCasts());
    }

    if (cfn && !cfn->isIntrinsic()) {
        return cfn;
    }

    return nullptr;
}

namespace klee {

PatchExplorer::PatchExplorer(Executor *executor)
    : mainModule(executor->kmodule->module.get()),
      cmpModule(executor->cmpModule->module.get()) {

    // STEP 1: map from pointers to basic blocks in main module to their weights
    std::unordered_map<llvm::BasicBlock *, int> bbweights;
    
    for (llvm::Function &func : *mainModule) {
        // llvm::errs() << "Function: " << func.getName().str() << "\n";
        llvm::Function *cmpFunc = cmpModule->getFunction(func.getName());
        if (cmpFunc == nullptr) {
            if (DEBUG_PRINTS) {
                llvm::errs() << "Function: " << func.getName().str() << " does NOT exist in original bitcode\n";
            }

            // HACK: temporarily assign each BB a weight of 1 if the functions differ
            for (llvm::BasicBlock &bb : func) {
                bbweights[&bb] = 1;
            }
            continue;
        }

        // iterate through all instructions ignored debug instructions
        auto mainFuncIter = func.begin();
        auto cmpFuncIter = cmpFunc->begin();

        while(mainFuncIter != func.end()) {

            llvm::BasicBlock *mainBB = &*mainFuncIter;

            // if we run out of BBs in cmpFuncIter, mark rest of BBs in mainFunc as differing
            if (cmpFuncIter == cmpFunc->end()) {
                bbweights[mainBB] = 1;

                if (DEBUG_PRINTS) {
                    llvm::errs() << "BB: ";
                    mainBB->printAsOperand(llvm::errs(), false);
                    llvm::errs() << " in function: " << func.getName().str() << " marked as different because cmpFunc has no more BBs\n";
                }

                mainFuncIter++;
                continue; // to next func
            }

            // iterate through instructions in both BBs
            llvm::BasicBlock *cmpBB = &*cmpFuncIter;

            // iterator ranges
            auto mainBBIterRange = mainBB->instructionsWithoutDebug();
            auto cmpBBIterRange = cmpBB->instructionsWithoutDebug();
            auto mainBBIter = mainBBIterRange.begin();
            auto cmpBBIter = cmpBBIterRange.begin();

            while(mainBBIter != mainBBIterRange.end()) {

                // if we run out of instructions in the cmpBB, mark this BB as differing
                if (cmpBBIter == cmpBBIterRange.end()) {
                    bbweights[mainBB] = 1;
                    
                    if (DEBUG_PRINTS) {
                        llvm::errs() << "BB: ";
                        mainBB->printAsOperand(llvm::errs(), false);
                        llvm::errs() << " in function: " << func.getName().str() << " marked as different because cmpBB has no more insts\n";
                    }

                    break; // to next bb
                }
                
                // compare instructions from both
                std::string mainInstStr;
                llvm::raw_string_ostream(mainInstStr) << *mainBBIter;
                std::istringstream mainSS(mainInstStr);

                std::string cmpInstStr;
                llvm::raw_string_ostream(cmpInstStr) << *cmpBBIter;
                std::istringstream cmpSS(cmpInstStr);

                // llvm::errs() << mainInstStr << "  vs  " << cmpInstStr << "\n";
                if (!instructionsEqual(mainInstStr, cmpInstStr)) {
                    if (DEBUG_PRINTS) {
                        llvm::errs() << "BB: ";
                        mainBB->printAsOperand(llvm::errs(), false);
                        llvm::errs() << " in function: " << func.getName().str() << " marked as different because differing instructions:\n\t";
                        llvm::errs() << mainInstStr << " vs " << cmpInstStr << "\n";
                    }

                    bbweights[mainBB] = 1;
                    break; // to next bb
                }

                mainBBIter++;
                cmpBBIter++;
            }

            // if we got to this point, all the instructions in both BBs are the same
            bbweights[mainBB] = 0;

            mainFuncIter++;
            cmpFuncIter++;
        }
    }

    // STEP 3: compute and back propagate priorities
    // This routine is modified from Agamotto
    // https://github.com/efeslab/agamotto/blob/artifact-eval-osdi20/lib/Core/NvmHeuristics.cpp
    std::unordered_set<llvm::CallBase*> call_insts;

    // initialize priorities and find call sites for back prop
    for (llvm::Function &f : *mainModule) {
        for (llvm::BasicBlock &b : f) {
            for (llvm::Instruction &i : b) {
                // init all priorities to 0
                priorities[&i] = 0;
                if (llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&i)) {
                    if (auto *ci = llvm::dyn_cast<llvm::CallInst>(cb)) {
                        if (ci->isInlineAsm()) continue;
                    } 
                    call_insts.insert(cb);
                }
            }
        }
    }

    for (llvm::Function &f : *mainModule) {
        if (f.empty()) continue;
        llvm::DominatorTree dom(f);

        // Find the ending basic blocks
        std::unordered_set<llvm::BasicBlock*> endBlocks, bbSet, traversed;
    
        llvm::BasicBlock *entry = &f.getEntryBlock();
        assert(entry);
        bbSet.insert(entry);

        while(bbSet.size()) {
            llvm::BasicBlock *bb = *bbSet.begin();
            assert(bb);
            bbSet.erase(bbSet.begin());
            traversed.insert(bb);

            if (llvm::succ_empty(bb)) {
                endBlocks.insert(bb);
            } else {
                for (llvm::BasicBlock *sbb : llvm::successors(bb)) {
                    assert(sbb);
                    if (traversed.count(sbb)) continue;
                    if (!dom.dominates(sbb, bb)) bbSet.insert(sbb);
                }
            }
        }

        // errs() << "\tfound terminators" << "\n";
        bbSet = endBlocks;

        /**
        * Propagating the priority is slightly trickier than just finding the 
        * terminal basic blocks, as different paths can have different priorities.
        * So, we annotate the propagated with the priority. If the priority changed,
        * then reprop.
        */

        std::unordered_map<llvm::BasicBlock*, uint64_t> prop;

        while (bbSet.size()) {
            llvm::BasicBlock *bb = *bbSet.begin();
            assert(bb);
            bbSet.erase(bbSet.begin());

            llvm::Instruction *pi = bb->getTerminator();
            if (!pi) {
                assert(f.isDeclaration());
                continue; // empty body
            }

            if (prop.count(bb) && prop[bb] == priorities[pi]) {
                continue;
            }
            prop[bb] = priorities[pi];

            llvm::Instruction *i = pi->getPrevNode();
            while (i) {
                priorities[i] = priorities[pi] + bbweights[bb];
                pi = i;
                i = pi->getPrevNode();
            }

            for (llvm::BasicBlock *pbb : llvm::predecessors(bb)) {
                assert(pbb);
                // if (!dom.dominates(bb, pbb)) bbSet.insert(pbb);
                // bbSet.insert(pbb);
                if (!prop.count(pbb)) bbSet.insert(pbb);

                llvm::Instruction *term = pbb->getTerminator();
                priorities[term] = std::max(priorities[term], 
                                            bbweights[bb] + priorities[pi]); 
            }
        }
    }

    /**
    * We're actually still not done. For each function, the base priority needs to
    * be boosted by the priority of the call site return locations.
    */
    bool changed = false;
    do {
        changed = false;
        for (llvm::Function &f : *mainModule) {
            if (f.empty()) continue;
            for (llvm::Use &u : f.uses()) {
                llvm::User *usr = u.getUser();
                // errs() << "usr:" << *usr << "\n";
                if (auto *cb = llvm::dyn_cast<llvm::CallBase>(usr)) {
                    // errs() << "\t=> " << priorities[i] << "\n";
                    llvm::Instruction *retLoc = getReturnLocation(cb);
                    assert(retLoc);
                    // errs() << "\t=> " << priorities[retLoc] << "\n";
                    if (priorities[retLoc]) {
                        for (llvm::BasicBlock &bb : f) {
                            for (llvm::Instruction &ii : bb) {
                                if (!priorities[&ii]) {
                                    changed = true;
                                    priorities[&ii] = priorities[retLoc];
                                }
                            }
                        }
                    }
                }
            }
        }

        for (llvm::CallBase *cb : call_insts) {
            llvm::Instruction *retLoc = getReturnLocation(cb);
            assert(retLoc);

            std::unordered_set<llvm::Function*> possibleFns;
            if (llvm::Function *f = getCallInstFunction(cb)) {
                possibleFns.insert(f);
            } else if (llvm::Function *f = cb->getCalledFunction()) {
                possibleFns.insert(f);
            } else if (llvm::GlobalAlias *ga = llvm::dyn_cast<llvm::GlobalAlias>(cb->getCalledOperand())) {
                llvm::Function *f = llvm::dyn_cast<llvm::Function>(ga->getAliasee());
                assert(f && "bad assumption about aliases!");
                possibleFns.insert(f);
            } else if (llvm::GlobalAlias *ga = llvm::dyn_cast<llvm::GlobalAlias>(cb->getCalledOperand()->stripPointerCasts())) {
                llvm::Function *f = llvm::dyn_cast<llvm::Function>(ga->getAliasee());
                assert(f && "bad assumption about aliases!");
                possibleFns.insert(f);
            } else {
                if (!cb->isIndirectCall()) llvm::errs() << *cb << "\n";
                assert(cb->isIndirectCall());

                for (llvm::Function &f : *mainModule) {
                    for (unsigned i = 0; i < (unsigned)cb->arg_size(); ++i) {
                        if (f.arg_size() <= i) {
                            if (f.isVarArg()) {
                                possibleFns.insert(&f);
                            }
                            break;
                        }

                        llvm::Argument *arg = f.arg_begin() + i;
                        llvm::Value *val = cb->getArgOperand(i);

                        if (arg->getType() != val->getType()) break;
                        else if (i + 1 == cb->getNumOperands()) possibleFns.insert(&f);
                    }
                }
            }

            for (llvm::Function *f : possibleFns) {
                for (llvm::BasicBlock &bb : *f) {
                    for (llvm::Instruction &i : bb) {
                        if (!priorities[&i] && priorities[retLoc]) {
                            changed = true;
                            priorities[&i] = priorities[retLoc];
                        }
                    }
                }
            }
        }
    } while (changed);

    // dumpPriorities();
}

uint64_t PatchExplorer::get_priority(llvm::Instruction *inst) {
    return priorities[inst];
}

void PatchExplorer::dumpPriorities() {
    for (llvm::Function &func : *mainModule) {
        bool printedFunc = false;

        for (llvm::BasicBlock &bb : func) {
            bool printedBB = false;

            for (llvm::Instruction &inst : bb) {
                auto p = priorities[&inst];

                if (p != 0) {
                    if (!printedFunc) {
                        llvm::errs() << "Function: " << func.getName().str() << "\n";
                        printedFunc = true;
                    }

                    if (!printedBB) {
                        llvm::errs() << "\tBB: ";
                        bb.printAsOperand(llvm::errs(), false);
                        llvm::errs() << "\n";
                        printedBB = true;
                    }

                    llvm::errs() << "\t\t[" << p << "]: " << inst << "\n";
                }
            }
        }
    }
}

}