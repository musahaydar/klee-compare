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
// here, we ignore metadata as well as labeled operands, those we compare for equivalence even if they don't match
bool instructionStringsEquiv(std::string inst1, std::string inst2) {
    std::stringstream inst1ss(inst1);
    std::stringstream inst2ss(inst2);
    std::string substr1 = "", substr2 = "";

    while(inst1ss >> substr1) {
        inst2ss >> substr2;
        // ignore metadata
        // TODO: this can be made more forgiving (e.g. if one instruction has metadata, but the other doesn't)
        if (substr1[0] == '!' && substr2[0] == '!') {
            continue;
        }

        // ignore labeled operands
        if (substr1[0] == '%' && substr2[0] == '%') {
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

// recursively check that two instructions are equivalent (i.e. they are identical except for operand names,
// and their operands are defined in equivalent instructions)
bool instructionsEquiv(llvm::Instruction *inst1, llvm::Instruction *inst2, std::unordered_map<llvm::Instruction *, llvm::Instruction *> &memo) {
    // branches we should compare for equivalent bb targets, not using this function
    assert(!llvm::isa<llvm::BranchInst>(inst1));

    // check the memo first to avoid redundant calls
    if (memo.count(inst1) != 0 && memo[inst1] == inst2) {
        return true;
    }

    // check if the two instructions are syntactically equivalent
    std::string inst1string, inst2string;
    llvm::raw_string_ostream(inst1string) << *inst1;
    llvm::raw_string_ostream(inst2string) << *inst2;

    if (!instructionStringsEquiv(inst1string, inst2string)) {
        if (DEBUG_PRINTS) {
            llvm::errs() << "Inst: " << inst1string << " vs " << inst2string << " not equiv: strings\n";
        }

        return false;
    }

    // check that all operands are defined in equivalent instructions
    assert(inst1->getNumOperands() == inst2->getNumOperands() && "this shouldn't happen (instructions have different num operands)");

    for (unsigned int op = 0; op < inst1->getNumOperands(); ++op) {
        llvm::Value *inst1op = inst1->getOperand(op);
        llvm::Value *inst2op = inst2->getOperand(op);

        if (llvm::isa<llvm::Constant>(inst1op)) {
            if(!llvm::isa<llvm::Constant>(inst2op)) {
                // cutting a corner here by requiring the operands appear in the same order
                // so unless both are constants (we've already determined they're equal in the string)
                // we'll say the instructions are not equivalent
                return false;
            }
            continue;
        }

        llvm::Instruction *inst1opDef = llvm::dyn_cast<llvm::Instruction>(inst1op);
        llvm::Instruction *inst2opDef = llvm::dyn_cast<llvm::Instruction>(inst2op);

        if (inst1opDef == nullptr) {
            assert(inst2opDef == nullptr);
            continue; // to next operand
        }

        bool res = instructionsEquiv(inst1opDef, inst2opDef, memo);
        if (!res) {
            return false;
        }
    }

    // at this point, we've determined that the instructions are equivalent
    memo[inst1] = inst2;
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

    // STEP 1: compute weights of BBs
    std::unordered_map<llvm::BasicBlock *, int> bbweights;

    // auto curr = mainModule->getFunction(executor->entryPoint)->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();

    // map of equivalence between BBs (patched -> original) for checking control flow
    // use a set for the case of multiple equivalent blocks
    std::unordered_map<llvm::BasicBlock *, std::unordered_set<llvm::BasicBlock *>> bbEquivSets;
    
    for (llvm::Function &func : *mainModule) {
        // llvm::errs() << "Function: " << func.getName().str() << "\n";
        llvm::Function *cmpFunc = cmpModule->getFunction(func.getName());
        if (cmpFunc == nullptr) {
            if (DEBUG_PRINTS) {
                llvm::errs() << "Function: " << func.getName().str() << " does NOT exist in original bitcode\n";
            }

            // if this function is new, assign all BBs a weight of 1
            for (llvm::BasicBlock &bb : func) {
                bbweights[&bb] = 1;
            }
            continue; // to next function
        }
        
        // memo used for recursively checking equivalence in instructions, instantiated per function
        std::unordered_map<llvm::Instruction *, llvm::Instruction *> instEquivMemo;

        // for each BasicBlock, check every block in the cmpFunc and see if any are equivalent
        for (llvm::BasicBlock &bb : func) {
            // initialize weight to be 1 in case we don't find an equiv BB
            bbweights[&bb] = 1;

            for (llvm::BasicBlock &cmpBB : *cmpFunc) {
                // init to true, set to false if this BB we're checking was not equiv
                bool foundEquiv = true;

                // iterate through instructions in both BBs, ignoring debug
                auto mainBBIter = bb.instructionsWithoutDebug().begin();
                auto cmpBBIter = cmpBB.instructionsWithoutDebug().begin();

                while(mainBBIter != bb.instructionsWithoutDebug().end()) {
                    // if we run out of instructions in cmpBB before the main bb
                    // need to check this first!
                    if (cmpBBIter == cmpBB.instructionsWithoutDebug().end()) {
                        foundEquiv = false;
                        break; // to next bb in cmpFunc
                    }

                    // compare each instruction for equivalence except the terminator (branch)
                    // TODO: also for return instructions(?)
                    if (llvm::isa<llvm::BranchInst>(*mainBBIter)) { 
                        if (!llvm::isa<llvm::BranchInst>(*cmpBBIter)) {
                            foundEquiv = false;
                            break; // to next bb in cmpFunc
                        }

                        // reached branch in both basic blocks
                        mainBBIter++;
                        cmpBBIter++;
                        continue; // to next instruciton in bb (should exit loop)
                    }

                    // if instructions are not equivalent, break to next BB
                    if (!instructionsEquiv(&*mainBBIter, &*cmpBBIter, instEquivMemo)) {
                        foundEquiv = false;
                        break; // to next bb in cmpFunc
                    }

                    mainBBIter++;
                    cmpBBIter++;
                }

                // if there are more instructions in cmpBB, they aren't equivalent
                if(cmpBBIter != cmpBB.instructionsWithoutDebug().end()) {
                    foundEquiv = false;
                }
                
                if (foundEquiv) {
                    bbEquivSets[&bb].insert(&cmpBB);
                    bbweights[&bb] = 0;
                    // break; // to next bb in mainFunc
                }
            }
        }
    }

    // next, now we need to make another pass through all the BBs with equivalences and check that their
    // control flow is also equivalent
    for (auto iter : bbEquivSets) {
        bool foundEquiv = false;

        // we check all equivalent BBs ensuring that there's at least one with equivalent control flow too
        for (auto cmpIter : iter.second) {
            auto bbSuccIter = llvm::succ_begin(iter.first);
            auto cmpBBSuccIter = llvm::succ_begin(cmpIter);
            bool skip = false;

            while (bbSuccIter != llvm::succ_end(iter.first)) {
                // check if we are out of successors of cmpBB (differing control flow)
                if(cmpBBSuccIter == llvm::succ_end(cmpIter)) {
                    skip = true;
                    break; // to next equiv cmpBB
                }

                if (bbEquivSets.count(*bbSuccIter) == 0 || bbEquivSets[*bbSuccIter].count(*cmpBBSuccIter) == 0) {
                    // the control flow has changed for this basic block by the patch
                    skip = true;
                    break; // to next equiv cmpBB
                }
                ++bbSuccIter;
                ++cmpBBSuccIter;
            }

            // check if there are more successors of cmpBB (differing control flow)
            if(cmpBBSuccIter != llvm::succ_end(cmpIter) || skip) {
                continue; // to next equiv cmpBB
            }

            // if we made it through all the checks, we found an equiv BB and we're done
            foundEquiv = true;
            break;
        }

        if (!foundEquiv) {
            // TODO: do we want to give weight to this BB as differening because of the changed branch
            // or is the differing successor what we want to explore?
            bbweights[iter.first] = 1;
        }
    }

    // STEP 2: compute and back propagate priorities
    // This routine is modified from Agamotto
    // https://github.com/efeslab/agamotto/blob/artifact-eval-osdi20/lib/Core/NvmHeuristics.cpp

    // we'll map the computed weights to instructions as well, since we need the granularity for call sites
    std::unordered_map<llvm::Instruction *, int> instweights;

    std::unordered_set<llvm::CallBase*> call_insts;

    for (llvm::Function &f : *mainModule) {
        for (llvm::BasicBlock &b : f) {
            for (llvm::Instruction &i : b) {
                // init all priorities to 0
                priorities[&i] = 0;

                // set the weight of the instruction
                // we want to prioritize exploring the most differing code first, so we can just multiply
                // the weight by the size of the BB to achieve this (the weight should be a 0 or 1)
                // instweights[&i] = bbweights[&b] * b.size();

                instweights[&i] = bbweights[&b];

                // use this to dumpPriorities() with weights as priorities
                // priorities[&i] = instweights[&i];

                // store pointers to call sites
                if (llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&i)) {
                    if (auto *ci = llvm::dyn_cast<llvm::CallInst>(cb)) {
                        if (ci->isInlineAsm()) continue;
                    } 
                    call_insts.insert(cb);
                }
            }
        }
    }

    // We also need to fill in the weights for function calls.
    // We'll just give a weight of one, prioritizes immediate instructions.
    bool c = false;
    do {
        c = false;
        for (llvm::CallBase *cb : call_insts) {
            assert(cb && "callbase is nullptr!");
            std::unordered_set<llvm::Function *> possibleFns;
            // errs() << *cb << "\n";
            auto *tmp = llvm::dyn_cast<llvm::CallBase>(cb->stripPointerCasts());
            if (tmp) {
                cb = tmp;
            }
            assert(cb && "could not strip!");
            if (llvm::Function *f = getCallInstFunction(cb)) {
                possibleFns.insert(f);
            } else if (llvm::Function *f = cb->getCalledFunction()) {
                possibleFns.insert(f);
            } else if (auto *f = llvm::dyn_cast<llvm::Function>(cb->getCalledOperand()->stripPointerCasts())) {
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
                if (!cb->isIndirectCall()) {
                    // llvm::errs() << *cb << "\n";
                    // llvm::errs() << cb->getCalledOperand() << "\n";
                    // if (cb->getCalledOperand()) llvm::errs() << *cb->getCalledOperand() << "\n";
                    // if (cb->getCalledOperand()) llvm::errs() << *cb->getCalledOperand()->stripPointerCastsNoFollowAliases() << "\n";
                    // if (cb->getCalledOperand()) llvm::errs() << llvm::dyn_cast<llvm::GlobalAlias>(cb->getCalledOperand()->stripPointerCastsNoFollowAliases()) << "\n";
                    assert(false && "TODO");
                }
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
                        else if (i + 1 == cb->arg_size()) possibleFns.insert(&f);
                    }
                }
            }

            for (llvm::Function *f : possibleFns) {
                for (llvm::BasicBlock &bb : *f) {
                    for (llvm::Instruction &i : bb) {
                        if (instweights[&i] && !instweights[llvm::dyn_cast<llvm::Instruction>(cb)]) {
                            c = true;
                            instweights[llvm::dyn_cast<llvm::Instruction>(cb)] = 1;
                            goto done;
                        }
                    }
                }
            }

            done: (void)0;
        }
    } while (c);

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
                priorities[i] = priorities[pi] + instweights[i];
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
                                            instweights[term] + priorities[pi]); 
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
                        else if (i + 1 == cb->arg_size()) possibleFns.insert(&f);
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

uint64_t PatchExplorer::getPriority(llvm::Instruction *inst) {
    return (priorities.count(inst) ? priorities.at(inst) : 0);
    // assert(priorities.count(inst) && " instruction has no priority");
    // return priorities.at(inst);
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

void PatchExplorer::dumpProgram() {
    for (llvm::Function &func : *mainModule) {
        llvm::errs() << "Function: " << func.getName().str() << "\n";

        for (llvm::BasicBlock &bb : func) {
            llvm::errs() << "\tBB: ";
            bb.printAsOperand(llvm::errs(), false);
            llvm::errs() << "\n";
            

            for (llvm::Instruction &inst : bb) {
                llvm::errs() << "\t\t" << inst << "\n";
            }
        }
    }
}

}