//===-- SafepointIRVerifier.cpp - Verify gc.statepoint invariants ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Run a sanity check on the IR to ensure that Safepoints - if they've been
// inserted - were inserted correctly.  In particular, look for use of
// non-relocated values after a safepoint.  It's primary use is to check the
// correctness of safepoint insertion immediately after insertion, but it can
// also be used to verify that later transforms have not found a way to break
// safepoint semenatics.
//
// In its current form, this verify checks a property which is sufficient, but
// not neccessary for correctness.  There are some cases where an unrelocated
// pointer can be used after the safepoint.  Consider this example:
//
//    a = ...
//    b = ...
//    (a',b') = safepoint(a,b)
//    c = cmp eq a b
//    br c, ..., ....
//
// Because it is valid to reorder 'c' above the safepoint, this is legal.  In
// practice, this is a somewhat uncommon transform, but CodeGenPrep does create
// idioms like this.  The verifier knows about these cases and avoids reporting
// false positives.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "safepoint-ir-verifier"

using namespace llvm;

/// This option is used for writing test cases.  Instead of crashing the program
/// when verification fails, report a message to the console (for FileCheck
/// usage) and continue execution as if nothing happened.
static cl::opt<bool> PrintOnly("safepoint-ir-verifier-print-only",
                               cl::init(false));

static void Verify(const Function &F, const DominatorTree &DT);

namespace {
struct SafepointIRVerifier : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  DominatorTree DT;
  SafepointIRVerifier() : FunctionPass(ID) {
    initializeSafepointIRVerifierPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    DT.recalculate(F);
    Verify(F, DT);
    return false; // no modifications
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  StringRef getPassName() const override { return "safepoint verifier"; }
};
} // namespace

void llvm::verifySafepointIR(Function &F) {
  SafepointIRVerifier pass;
  pass.runOnFunction(F);
}

char SafepointIRVerifier::ID = 0;

FunctionPass *llvm::createSafepointIRVerifierPass() {
  return new SafepointIRVerifier();
}

INITIALIZE_PASS_BEGIN(SafepointIRVerifier, "verify-safepoint-ir",
                      "Safepoint IR Verifier", false, true)
INITIALIZE_PASS_END(SafepointIRVerifier, "verify-safepoint-ir",
                    "Safepoint IR Verifier", false, true)

static bool isGCPointerType(Type *T) {
  if (auto *PT = dyn_cast<PointerType>(T))
    // For the sake of this example GC, we arbitrarily pick addrspace(1) as our
    // GC managed heap.  We know that a pointer into this heap needs to be
    // updated and that no other pointer does.
    return (1 == PT->getAddressSpace());
  return false;
}

static bool containsGCPtrType(Type *Ty) {
  if (isGCPointerType(Ty))
    return true;
  if (VectorType *VT = dyn_cast<VectorType>(Ty))
    return isGCPointerType(VT->getScalarType());
  if (ArrayType *AT = dyn_cast<ArrayType>(Ty))
    return containsGCPtrType(AT->getElementType());
  if (StructType *ST = dyn_cast<StructType>(Ty))
    return std::any_of(ST->subtypes().begin(), ST->subtypes().end(),
                       containsGCPtrType);
  return false;
}

// Debugging aid -- prints a [Begin, End) range of values.
template<typename IteratorTy>
static void PrintValueSet(raw_ostream &OS, IteratorTy Begin, IteratorTy End) {
  OS << "[ ";
  while (Begin != End) {
    OS << **Begin << " ";
    ++Begin;
  }
  OS << "]";
}

/// The verifier algorithm is phrased in terms of availability.  The set of
/// values "available" at a given point in the control flow graph is the set of
/// correctly relocated value at that point, and is a subset of the set of
/// definitions dominating that point.

using AvailableValueSet = DenseSet<const Value *>;

/// State we compute and track per basic block.
struct BasicBlockState {
  // Set of values available coming in, before the phi nodes
  AvailableValueSet AvailableIn;

  // Set of values available going out
  AvailableValueSet AvailableOut;

  // AvailableOut minus AvailableIn.
  // All elements are Instructions
  AvailableValueSet Contribution;

  // True if this block contains a safepoint and thus AvailableIn does not
  // contribute to AvailableOut.
  bool Cleared = false;
};

/// A given derived pointer can have multiple base pointers through phi/selects.
/// This type indicates when the base pointer is exclusively constant
/// (ExclusivelySomeConstant), and if that constant is proven to be exclusively
/// null, we record that as ExclusivelyNull. In all other cases, the BaseType is
/// NonConstant.
enum BaseType {
  NonConstant = 1, // Base pointers is not exclusively constant.
  ExclusivelyNull,
  ExclusivelySomeConstant // Base pointers for a given derived pointer is from a
                          // set of constants, but they are not exclusively
                          // null.
};

/// Return the baseType for Val which states whether Val is exclusively
/// derived from constant/null, or not exclusively derived from constant.
/// Val is exclusively derived off a constant base when all operands of phi and
/// selects are derived off a constant base.
static enum BaseType getBaseType(const Value *Val) {

  SmallVector<const Value *, 32> Worklist;
  DenseSet<const Value *> Visited;
  bool isExclusivelyDerivedFromNull = true;
  Worklist.push_back(Val);
  // Strip through all the bitcasts and geps to get base pointer. Also check for
  // the exclusive value when there can be multiple base pointers (through phis
  // or selects).
  while(!Worklist.empty()) {
    const Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    if (const auto *CI = dyn_cast<CastInst>(V)) {
      Worklist.push_back(CI->stripPointerCasts());
      continue;
    }
    if (const auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
      Worklist.push_back(GEP->getPointerOperand());
      continue;
    }
    // Push all the incoming values of phi node into the worklist for
    // processing.
    if (const auto *PN = dyn_cast<PHINode>(V)) {
      for (Value *InV: PN->incoming_values())
        Worklist.push_back(InV);
      continue;
    }
    if (const auto *SI = dyn_cast<SelectInst>(V)) {
      // Push in the true and false values
      Worklist.push_back(SI->getTrueValue());
      Worklist.push_back(SI->getFalseValue());
      continue;
    }
    if (isa<Constant>(V)) {
      // We found at least one base pointer which is non-null, so this derived
      // pointer is not exclusively derived from null.
      if (V != Constant::getNullValue(V->getType()))
        isExclusivelyDerivedFromNull = false;
      // Continue processing the remaining values to make sure it's exclusively
      // constant.
      continue;
    }
    // At this point, we know that the base pointer is not exclusively
    // constant.
    return BaseType::NonConstant;
  }
  // Now, we know that the base pointer is exclusively constant, but we need to
  // differentiate between exclusive null constant and non-null constant.
  return isExclusivelyDerivedFromNull ? BaseType::ExclusivelyNull
                                      : BaseType::ExclusivelySomeConstant;
}

static bool isNotExclusivelyConstantDerived(const Value *V) {
  return getBaseType(V) == BaseType::NonConstant;
}

namespace {
class InstructionVerifier;

/// Builds BasicBlockState for each BB of the function.
/// It can traverse function for verification and provides all required
/// information.
class GCPtrTracker {
  const Function &F;
  SpecificBumpPtrAllocator<BasicBlockState> BSAllocator;
  DenseMap<const BasicBlock *, BasicBlockState *> BlockMap;
  // This set contains defs of unrelocated pointers that are proved to be legal
  // and don't need verification.
  DenseSet<const Instruction *> ValidUnrelocatedDefs;

public:
  GCPtrTracker(const Function &F, const DominatorTree &DT);

  BasicBlockState *getBasicBlockState(const BasicBlock *BB);
  const BasicBlockState *getBasicBlockState(const BasicBlock *BB) const;

  /// Traverse each BB of the function and call
  /// InstructionVerifier::verifyInstruction for each possibly invalid
  /// instruction.
  /// It destructively modifies GCPtrTracker so it's passed via rvalue reference
  /// in order to prohibit further usages of GCPtrTracker as it'll be in
  /// inconsistent state.
  static void verifyFunction(GCPtrTracker &&Tracker,
                             InstructionVerifier &Verifier);

private:
  /// Returns true if the instruction may be safely skipped during verification.
  bool instructionMayBeSkipped(const Instruction *I) const;

  /// Iterates over all BBs from BlockMap and recalculates AvailableIn/Out for
  /// each of them until it converges.
  void recalculateBBsStates();

  /// Remove from Contribution all defs that legally produce unrelocated
  /// pointers and saves them to ValidUnrelocatedDefs.
  /// Though Contribution should belong to BBS it is passed separately with
  /// different const-modifier in order to emphasize (and guarantee) that only
  /// Contribution will be changed.
  /// Returns true if Contribution was changed otherwise false.
  bool removeValidUnrelocatedDefs(const BasicBlock *BB,
                                  const BasicBlockState *BBS,
                                  AvailableValueSet &Contribution);

  /// Gather all the definitions dominating the start of BB into Result. This is
  /// simply the defs introduced by every dominating basic block and the
  /// function arguments.
  void gatherDominatingDefs(const BasicBlock *BB, AvailableValueSet &Result,
                            const DominatorTree &DT);

  /// Compute the AvailableOut set for BB, based on the BasicBlockState BBS,
  /// which is the BasicBlockState for BB.
  /// ContributionChanged is set when the verifier runs for the first time
  /// (in this case Contribution was changed from 'empty' to its initial state)
  /// or when Contribution of this BB was changed since last computation.
  static void transferBlock(const BasicBlock *BB, BasicBlockState &BBS,
                            bool ContributionChanged);

  /// Model the effect of an instruction on the set of available values.
  static void transferInstruction(const Instruction &I, bool &Cleared,
                                  AvailableValueSet &Available);
};

/// It is a visitor for GCPtrTracker::verifyFunction. It decides if the
/// instruction (which uses heap reference) is legal or not, given our safepoint
/// semantics.
class InstructionVerifier {
  bool AnyInvalidUses = false;

public:
  void verifyInstruction(const GCPtrTracker *Tracker, const Instruction &I,
                         const AvailableValueSet &AvailableSet);

  bool hasAnyInvalidUses() const { return AnyInvalidUses; }

private:
  void reportInvalidUse(const Value &V, const Instruction &I);
};
} // end anonymous namespace

GCPtrTracker::GCPtrTracker(const Function &F, const DominatorTree &DT) : F(F) {
  // First, calculate Contribution of each BB.
  for (const BasicBlock &BB : F) {
    BasicBlockState *BBS = new (BSAllocator.Allocate()) BasicBlockState;
    for (const auto &I : BB)
      transferInstruction(I, BBS->Cleared, BBS->Contribution);
    BlockMap[&BB] = BBS;
  }

  // Initialize AvailableIn/Out sets of each BB using only information about
  // dominating BBs.
  for (auto &BBI : BlockMap) {
    gatherDominatingDefs(BBI.first, BBI.second->AvailableIn, DT);
    transferBlock(BBI.first, *BBI.second, true);
  }

  // Simulate the flow of defs through the CFG and recalculate AvailableIn/Out
  // sets of each BB until it converges. If any def is proved to be an
  // unrelocated pointer, it will be removed from all BBSs.
  recalculateBBsStates();
}

BasicBlockState *GCPtrTracker::getBasicBlockState(const BasicBlock *BB) {
  auto it = BlockMap.find(BB);
  assert(it != BlockMap.end() &&
         "No such BB in BlockMap! Probably BB from another function");
  return it->second;
}

const BasicBlockState *GCPtrTracker::getBasicBlockState(
    const BasicBlock *BB) const {
  return const_cast<GCPtrTracker *>(this)->getBasicBlockState(BB);
}

bool GCPtrTracker::instructionMayBeSkipped(const Instruction *I) const {
  return ValidUnrelocatedDefs.count(I);
}

void GCPtrTracker::verifyFunction(GCPtrTracker &&Tracker,
                                  InstructionVerifier &Verifier) {
  // We need RPO here to a) report always the first error b) report errors in
  // same order from run to run.
  ReversePostOrderTraversal<const Function *> RPOT(&Tracker.F);
  for (const BasicBlock *BB : RPOT) {
    BasicBlockState *BBS = Tracker.getBasicBlockState(BB);
    // We destructively modify AvailableIn as we traverse the block instruction
    // by instruction.
    AvailableValueSet &AvailableSet = BBS->AvailableIn;
    for (const Instruction &I : *BB) {
      if (Tracker.instructionMayBeSkipped(&I))
        continue; // This instruction shouldn't be added to AvailableSet.

      Verifier.verifyInstruction(&Tracker, I, AvailableSet);

      // Model the effect of current instruction on AvailableSet to keep the set
      // relevant at each point of BB.
      bool Cleared = false;
      transferInstruction(I, Cleared, AvailableSet);
      (void)Cleared;
    }
  }
}

void GCPtrTracker::recalculateBBsStates() {
  SetVector<const BasicBlock *> Worklist;
  // TODO: This order is suboptimal, it's better to replace it with priority
  // queue where priority is RPO number of BB.
  for (auto &BBI : BlockMap)
    Worklist.insert(BBI.first);

  // This loop iterates the AvailableIn/Out sets until it converges.
  // The AvailableIn and AvailableOut sets decrease as we iterate.
  while (!Worklist.empty()) {
    const BasicBlock *BB = Worklist.pop_back_val();
    BasicBlockState *BBS = BlockMap[BB];

    size_t OldInCount = BBS->AvailableIn.size();
    for (const BasicBlock *PBB : predecessors(BB))
      set_intersect(BBS->AvailableIn, BlockMap[PBB]->AvailableOut);

    assert(OldInCount >= BBS->AvailableIn.size() && "invariant!");

    bool InputsChanged = OldInCount != BBS->AvailableIn.size();
    bool ContributionChanged =
        removeValidUnrelocatedDefs(BB, BBS, BBS->Contribution);
    if (!InputsChanged && !ContributionChanged)
      continue;

    size_t OldOutCount = BBS->AvailableOut.size();
    transferBlock(BB, *BBS, ContributionChanged);
    if (OldOutCount != BBS->AvailableOut.size()) {
      assert(OldOutCount > BBS->AvailableOut.size() && "invariant!");
      Worklist.insert(succ_begin(BB), succ_end(BB));
    }
  }
}

bool GCPtrTracker::removeValidUnrelocatedDefs(const BasicBlock *BB,
                                              const BasicBlockState *BBS,
                                              AvailableValueSet &Contribution) {
  assert(&BBS->Contribution == &Contribution &&
         "Passed Contribution should be from the passed BasicBlockState!");
  AvailableValueSet AvailableSet = BBS->AvailableIn;
  bool ContributionChanged = false;
  for (const Instruction &I : *BB) {
    bool ProducesUnrelocatedPointer = false;
    if ((isa<GetElementPtrInst>(I) || isa<BitCastInst>(I)) &&
        containsGCPtrType(I.getType())) {
      // GEP/bitcast of unrelocated pointer is legal by itself but this
      // def shouldn't appear in any AvailableSet.
      for (const Value *V : I.operands())
        if (containsGCPtrType(V->getType()) &&
            isNotExclusivelyConstantDerived(V) && !AvailableSet.count(V)) {
          ProducesUnrelocatedPointer = true;
          break;
        }
    }
    if (!ProducesUnrelocatedPointer) {
      bool Cleared = false;
      transferInstruction(I, Cleared, AvailableSet);
      (void)Cleared;
    } else {
      // Remove def of unrelocated pointer from Contribution of this BB
      // and trigger update of all its successors.
      Contribution.erase(&I);
      ValidUnrelocatedDefs.insert(&I);
      DEBUG(dbgs() << "Removing " << I << " from Contribution of "
                   << BB->getName() << "\n");
      ContributionChanged = true;
    }
  }
  return ContributionChanged;
}

void GCPtrTracker::gatherDominatingDefs(const BasicBlock *BB,
                                        AvailableValueSet &Result,
                                        const DominatorTree &DT) {
  DomTreeNode *DTN = DT[const_cast<BasicBlock *>(BB)];

  while (DTN->getIDom()) {
    DTN = DTN->getIDom();
    const auto &Defs = BlockMap[DTN->getBlock()]->Contribution;
    Result.insert(Defs.begin(), Defs.end());
    // If this block is 'Cleared', then nothing LiveIn to this block can be
    // available after this block completes.  Note: This turns out to be
    // really important for reducing memory consuption of the initial available
    // sets and thus peak memory usage by this verifier.
    if (BlockMap[DTN->getBlock()]->Cleared)
      return;
  }

  for (const Argument &A : BB->getParent()->args())
    if (containsGCPtrType(A.getType()))
      Result.insert(&A);
}

void GCPtrTracker::transferBlock(const BasicBlock *BB, BasicBlockState &BBS,
                                 bool ContributionChanged) {
  const AvailableValueSet &AvailableIn = BBS.AvailableIn;
  AvailableValueSet &AvailableOut = BBS.AvailableOut;

  if (BBS.Cleared) {
    // AvailableOut will change only when Contribution changed.
    if (ContributionChanged)
      AvailableOut = BBS.Contribution;
  } else {
    // Otherwise, we need to reduce the AvailableOut set by things which are no
    // longer in our AvailableIn
    AvailableValueSet Temp = BBS.Contribution;
    set_union(Temp, AvailableIn);
    AvailableOut = std::move(Temp);
  }

  DEBUG(dbgs() << "Transfered block " << BB->getName() << " from ";
        PrintValueSet(dbgs(), AvailableIn.begin(), AvailableIn.end());
        dbgs() << " to ";
        PrintValueSet(dbgs(), AvailableOut.begin(), AvailableOut.end());
        dbgs() << "\n";);
}

void GCPtrTracker::transferInstruction(const Instruction &I, bool &Cleared,
                                       AvailableValueSet &Available) {
  if (isStatepoint(I)) {
    Cleared = true;
    Available.clear();
  } else if (containsGCPtrType(I.getType()))
    Available.insert(&I);
}

void InstructionVerifier::verifyInstruction(
    const GCPtrTracker *Tracker, const Instruction &I,
    const AvailableValueSet &AvailableSet) {
  if (const PHINode *PN = dyn_cast<PHINode>(&I)) {
    if (containsGCPtrType(PN->getType()))
      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
        const BasicBlock *InBB = PN->getIncomingBlock(i);
        const Value *InValue = PN->getIncomingValue(i);

        if (isNotExclusivelyConstantDerived(InValue) &&
            !Tracker->getBasicBlockState(InBB)->AvailableOut.count(InValue))
          reportInvalidUse(*InValue, *PN);
      }
  } else if (isa<CmpInst>(I) &&
             containsGCPtrType(I.getOperand(0)->getType())) {
    Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
    enum BaseType baseTyLHS = getBaseType(LHS),
                  baseTyRHS = getBaseType(RHS);

    // Returns true if LHS and RHS are unrelocated pointers and they are
    // valid unrelocated uses.
    auto hasValidUnrelocatedUse = [&AvailableSet, baseTyLHS, baseTyRHS, &LHS,
                                   &RHS] () {
        // A cmp instruction has valid unrelocated pointer operands only if
        // both operands are unrelocated pointers.
        // In the comparison between two pointers, if one is an unrelocated
        // use, the other *should be* an unrelocated use, for this
        // instruction to contain valid unrelocated uses. This unrelocated
        // use can be a null constant as well, or another unrelocated
        // pointer.
        if (AvailableSet.count(LHS) || AvailableSet.count(RHS))
          return false;
        // Constant pointers (that are not exclusively null) may have
        // meaning in different VMs, so we cannot reorder the compare
        // against constant pointers before the safepoint. In other words,
        // comparison of an unrelocated use against a non-null constant
        // maybe invalid.
        if ((baseTyLHS == BaseType::ExclusivelySomeConstant &&
             baseTyRHS == BaseType::NonConstant) ||
            (baseTyLHS == BaseType::NonConstant &&
             baseTyRHS == BaseType::ExclusivelySomeConstant))
          return false;
        // All other cases are valid cases enumerated below:
        // 1. Comparison between an exlusively derived null pointer and a
        // constant base pointer.
        // 2. Comparison between an exlusively derived null pointer and a
        // non-constant unrelocated base pointer.
        // 3. Comparison between 2 unrelocated pointers.
        return true;
    };
    if (!hasValidUnrelocatedUse()) {
      // Print out all non-constant derived pointers that are unrelocated
      // uses, which are invalid.
      if (baseTyLHS == BaseType::NonConstant && !AvailableSet.count(LHS))
        reportInvalidUse(*LHS, I);
      if (baseTyRHS == BaseType::NonConstant && !AvailableSet.count(RHS))
        reportInvalidUse(*RHS, I);
    }
  } else {
    for (const Value *V : I.operands())
      if (containsGCPtrType(V->getType()) &&
          isNotExclusivelyConstantDerived(V) && !AvailableSet.count(V))
        reportInvalidUse(*V, I);
  }
}

void InstructionVerifier::reportInvalidUse(const Value &V,
                                           const Instruction &I) {
  errs() << "Illegal use of unrelocated value found!\n";
  errs() << "Def: " << V << "\n";
  errs() << "Use: " << I << "\n";
  if (!PrintOnly)
    abort();
  AnyInvalidUses = true;
}

static void Verify(const Function &F, const DominatorTree &DT) {
  DEBUG(dbgs() << "Verifying gc pointers in function: " << F.getName() << "\n");
  if (PrintOnly)
    dbgs() << "Verifying gc pointers in function: " << F.getName() << "\n";

  GCPtrTracker Tracker(F, DT);

  // We now have all the information we need to decide if the use of a heap
  // reference is legal or not, given our safepoint semantics.

  InstructionVerifier Verifier;
  GCPtrTracker::verifyFunction(std::move(Tracker), Verifier);

  if (PrintOnly && !Verifier.hasAnyInvalidUses()) {
    dbgs() << "No illegal uses found by SafepointIRVerifier in: " << F.getName()
           << "\n";
  }
}
