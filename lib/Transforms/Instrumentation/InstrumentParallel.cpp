//===-- InstrumentParallel.cpp - SWORD race detector -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of SWORD, an OpenMP race detector.
//
//===----------------------------------------------------------------------===//

#include "../../../include/sword/LinkAllPasses.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

#define DEBUG_TYPE "sword"

STATISTIC(NumInstrumentedReads, "Number of instrumented reads");
STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumOmittedReadsBeforeWrite,
          "Number of reads ignored due to following writes");
STATISTIC(NumAccessesWithBadSize, "Number of accesses with bad size");
STATISTIC(NumInstrumentedVtableWrites, "Number of vtable ptr writes");
STATISTIC(NumInstrumentedVtableReads, "Number of vtable ptr reads");
STATISTIC(NumOmittedReadsFromConstantGlobals,
          "Number of reads from constant globals");
STATISTIC(NumOmittedReadsFromVtable, "Number of vtable reads");
STATISTIC(NumOmittedNonCaptured, "Number of accesses ignored due to capturing");

//static const char *const kSwordModuleCtorName = "sword.module_ctor";
//static const char *const kSwordInitName = "__sword_init";

#define MIN_VERSION 39

static cl::opt<bool>  ClInstrumentMemoryAccesses(
    "sword-instrument-memory-accesses", cl::init(true),
    cl::desc("Instrument memory accesses"), cl::Hidden);
static cl::opt<bool>  ClInstrumentFuncEntryExit(
    "sword-instrument-func-entry-exit", cl::init(true),
    cl::desc("Instrument function entry and exit"), cl::Hidden);
static cl::opt<bool>  ClInstrumentAtomics(
    "sword-instrument-atomics", cl::init(true),
    cl::desc("Instrument atomics"), cl::Hidden);
static cl::opt<bool>  ClInstrumentMemIntrinsics(
    "sword-instrument-memintrinsics", cl::init(true),
    cl::desc("Instrument memintrinsics (memset/memcpy/memmove)"), cl::Hidden);

namespace {

struct InstrumentParallel : public FunctionPass {
  InstrumentParallel() : FunctionPass(ID) { PassName = "InstrumentParallel"; }
#if LLVM_VERSION > MIN_VERSION
  StringRef getPassName() const override;
#else
  const char *getPassName() const override;
#endif
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnFunction(Function &F) override;
  bool doInitialization(Module &M) override;
  static char ID;  // Pass identification, replacement for typeid.

private:
  void initializeCallbacks(Module &M);
  bool instrumentLoadOrStore(Instruction *I, const DataLayout &DL);
  bool instrumentAtomic(Instruction *I, const DataLayout &DL);
  bool instrumentMemIntrinsic(Instruction *I);
  void chooseInstructionsToInstrument(SmallVectorImpl<Instruction *> &Local,
                                      SmallVectorImpl<Instruction *> &All,
                                      const DataLayout &DL);
  bool addrPointsToConstantData(Value *Addr);
  int getMemoryAccessFuncIndex(Value *Addr, const DataLayout &DL);

  std::string PassName;
  void setMetadata(Instruction *Inst, const char *name, const char *description);
  bool isNotInstrumentable(MDNode *mdNode);
  Value *getPointerOperand(Value *I);

  Type *IntptrTy;
  IntegerType *OrdTy;
  // Callbacks to run-time library are computed in doInitialization.
  GlobalVariable *AccessMin;
  GlobalVariable *AccessMax;
  GlobalVariable *FunctionMin;
  GlobalVariable *FunctionMax;
  Function *SwordFuncEntry;
  Function *SwordFuncExit;
  Function *SwordFuncTermination;
  // Accesses sizes are powers of two: 1, 2, 4, 8, 16.
  static const size_t kNumberOfAccessSizes = 5;
  Function *SwordRead[kNumberOfAccessSizes];
  Function *SwordWrite[kNumberOfAccessSizes];
  Function *SwordUnalignedRead[kNumberOfAccessSizes];
  Function *SwordUnalignedWrite[kNumberOfAccessSizes];
  Function *SwordAtomicLoad[kNumberOfAccessSizes];
  Function *SwordAtomicStore[kNumberOfAccessSizes];
  Function *SwordAtomicRMW[AtomicRMWInst::LAST_BINOP + 1][kNumberOfAccessSizes];
  Function *SwordAtomicCAS[kNumberOfAccessSizes];
  Function *SwordAtomicThreadFence;
  Function *SwordAtomicSignalFence;
  Function *SwordVptrUpdate;
  Function *SwordVptrLoad;
  Function *MemmoveFn, *MemcpyFn, *MemsetFn;
  Function *SwordCtorFunction;
};
}  // namespace

char InstrumentParallel::ID = 0;
INITIALIZE_PASS_BEGIN(
    InstrumentParallel, "sword-sbl",
    "InstrumentParallel: instrument parallel functions.",
    false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(
    InstrumentParallel, "sword-sbl",
    "InstrumentParallel: instrument parallel functions.",
    false, false)

#if LLVM_VERSION > MIN_VERSION
	StringRef InstrumentParallel::getPassName() const {
		return PassName;
	}
#else
	const char *InstrumentParallel::getPassName() const {
		return PassName.c_str();
	}
#endif

void InstrumentParallel::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
}

Pass *llvm::createInstrumentParallelPass() {
  return new InstrumentParallel();
}

void InstrumentParallel::initializeCallbacks(Module &M) {
  IRBuilder<> IRB(M.getContext());
  // Initialize the callbacks.
  SwordFuncEntry = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__sword_func_entry", IRB.getVoidTy(), IRB.getInt64Ty(), IRB.getInt8PtrTy(), nullptr));
  SwordFuncExit = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__sword_func_exit", IRB.getVoidTy(), IRB.getInt64Ty(), nullptr));
  SwordFuncTermination = checkSanitizerInterfaceFunction(
    M.getOrInsertFunction("__sword_internal_end_checker_threads", IRB.getVoidTy(), nullptr));
  OrdTy = IRB.getInt32Ty();
  for (size_t i = 0; i < kNumberOfAccessSizes; ++i) {
    const unsigned ByteSize = 1U << i;
    const unsigned BitSize = ByteSize * 8;
    std::string ByteSizeStr = utostr(ByteSize);
    std::string BitSizeStr = utostr(BitSize);

    int LongSize = M.getDataLayout().getPointerSizeInBits();
    IntptrTy = Type::getIntNTy(M.getContext(), LongSize);

    SmallString<32> ReadName("__sword_read" + ByteSizeStr);
    SwordRead[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        ReadName, IRB.getVoidTy(), IRB.getInt8PtrTy(), IntptrTy, nullptr));

    SmallString<32> WriteName("__sword_write" + ByteSizeStr);
    SwordWrite[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        WriteName, IRB.getVoidTy(), IRB.getInt8PtrTy(), IntptrTy, nullptr));

    SmallString<64> UnalignedReadName("_sword_unaligned_read" + ByteSizeStr);
    SwordUnalignedRead[i] =
        checkSanitizerInterfaceFunction(M.getOrInsertFunction(
            UnalignedReadName, IRB.getVoidTy(), IRB.getInt8PtrTy(), nullptr));

    SmallString<64> UnalignedWriteName("__sword_unaligned_write" + ByteSizeStr);
    SwordUnalignedWrite[i] =
        checkSanitizerInterfaceFunction(M.getOrInsertFunction(
            UnalignedWriteName, IRB.getVoidTy(), IRB.getInt8PtrTy(), nullptr));

    Type *Ty = Type::getIntNTy(M.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    SmallString<32> AtomicLoadName("__sword_atomic" + BitSizeStr + "_load");
    SwordAtomicLoad[i] = checkSanitizerInterfaceFunction(
        M.getOrInsertFunction(AtomicLoadName, Ty, PtrTy, OrdTy, nullptr));

    SmallString<32> AtomicStoreName("__sword_atomic" + BitSizeStr + "_store");
    SwordAtomicStore[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        AtomicStoreName, IRB.getVoidTy(), PtrTy, Ty, OrdTy, nullptr));

    for (int op = AtomicRMWInst::FIRST_BINOP;
        op <= AtomicRMWInst::LAST_BINOP; ++op) {
      SwordAtomicRMW[op][i] = nullptr;
      const char *NamePart = nullptr;
      if (op == AtomicRMWInst::Xchg)
        NamePart = "_exchange";
      else if (op == AtomicRMWInst::Add)
        NamePart = "_fetch_add";
      else if (op == AtomicRMWInst::Sub)
        NamePart = "_fetch_sub";
      else if (op == AtomicRMWInst::And)
        NamePart = "_fetch_and";
      else if (op == AtomicRMWInst::Or)
        NamePart = "_fetch_or";
      else if (op == AtomicRMWInst::Xor)
        NamePart = "_fetch_xor";
      else if (op == AtomicRMWInst::Nand)
        NamePart = "_fetch_nand";
      else
        continue;
      SmallString<32> RMWName("__sword_atomic" + itostr(BitSize) + NamePart);
      SwordAtomicRMW[op][i] = checkSanitizerInterfaceFunction(
          M.getOrInsertFunction(RMWName, Ty, PtrTy, Ty, OrdTy, nullptr));
    }

    SmallString<32> AtomicCASName("__sword_atomic" + BitSizeStr +
                                  "_compare_exchange_val");
    SwordAtomicCAS[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        AtomicCASName, Ty, PtrTy, Ty, Ty, OrdTy, OrdTy, nullptr));
  }
  // SwordVptrUpdate = checkSanitizerInterfaceFunction(
  //     M.getOrInsertFunction("__sword_vptr_update", IRB.getVoidTy(),
  //                           IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));
  // SwordVptrLoad = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
  //     "__sword_vptr_read", IRB.getVoidTy(), IRB.getInt8PtrTy(), nullptr));
  SwordAtomicThreadFence = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__sword_atomic_thread_fence", IRB.getVoidTy(), OrdTy, nullptr));
  SwordAtomicSignalFence = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__sword_atomic_signal_fence", IRB.getVoidTy(), OrdTy, nullptr));

  // MemmoveFn = checkSanitizerInterfaceFunction(
  //     M.getOrInsertFunction("memmove", IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
  //                           IRB.getInt8PtrTy(), IntptrTy, nullptr));
  // MemcpyFn = checkSanitizerInterfaceFunction(
  //     M.getOrInsertFunction("memcpy", IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
  //                           IRB.getInt8PtrTy(), IntptrTy, nullptr));
  // MemsetFn = checkSanitizerInterfaceFunction(
  //     M.getOrInsertFunction("memset", IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
  //                           IRB.getInt32Ty(), IntptrTy, nullptr));
}

bool InstrumentParallel::doInitialization(Module &M) {
  PointerType *Int64PtrTy = IntegerType::getInt64PtrTy(M.getContext());
  ConstantInt *Zero64 = ConstantInt::get(Type::getInt64Ty(M.getContext()), 0);
  ConstantInt *UintMax64 = ConstantInt::get(Type::getInt64Ty(M.getContext()), UINTMAX_MAX);
  IRBuilder<> IRB(M.getContext());
  AccessMin = new llvm::GlobalVariable(M, IRB.getInt64Ty(), false,
                                       llvm::GlobalValue::PrivateLinkage, UintMax64,
                                       "access_min", NULL,
                                       GlobalVariable::NotThreadLocal, 0, false);
  AccessMax = new llvm::GlobalVariable(M, IRB.getInt64Ty(), false,
                                       llvm::GlobalValue::PrivateLinkage, Zero64,
                                       "access_max", NULL,
                                       GlobalVariable::NotThreadLocal, 0, false);
  FunctionMin = new llvm::GlobalVariable(M, IRB.getInt64Ty(), false,
                                       llvm::GlobalValue::PrivateLinkage, UintMax64,
                                       "function_min", NULL,
                                       GlobalVariable::NotThreadLocal, 0, false);
  FunctionMax = new llvm::GlobalVariable(M, IRB.getInt64Ty(), false,
                                       llvm::GlobalValue::PrivateLinkage, Zero64,
                                       "function_max", NULL,
                                       GlobalVariable::NotThreadLocal, 0, false);
//  std::tie(SwordCtorFunction, std::ignore) = createSanitizerCtorAndInitFunctions(
//      M, kSwordModuleCtorName, kSwordInitName,
//      /*InitArgTypes=*/{Int64PtrTy, Int64PtrTy, Int64PtrTy, Int64PtrTy},
//      /*InitArgs=*/{AccessMin, AccessMax, FunctionMin, FunctionMax});

//  appendToGlobalCtors(M, SwordCtorFunction, 0);
  return true;
}

static bool isVtableAccess(Instruction *I) {
  if (MDNode *Tag = I->getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

// Do not instrument known races/"benign races" that come from compiler
// instrumentatin. The user has no way of suppressing them.
static bool shouldInstrumentReadWriteFromAddress(Value *Addr) {
  // Peel off GEPs and BitCasts.
  Addr = Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->hasSection()) {
      StringRef SectionName = GV->getSection();
      // Check if the global is in the PGO counters section.
      if (SectionName.endswith(getInstrProfCountersSectionName(
            /*AddSegment=*/false)))
        return false;
    }

    // Check if the global is private gcov data.
    if (GV->getName().startswith("__llvm_gcov") ||
        GV->getName().startswith("__llvm_gcda"))
      return false;
  }

  // Do not instrument acesses from different address spaces; we cannot deal
  // with them.
  if (Addr) {
    Type *PtrTy = cast<PointerType>(Addr->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0)
      return false;
  }

  return true;
}

bool InstrumentParallel::addrPointsToConstantData(Value *Addr) {
  // If this is a GEP, just analyze its pointer operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Addr))
    Addr = GEP->getPointerOperand();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->isConstant()) {
      // Reads from constant globals can not race with any writes.
      NumOmittedReadsFromConstantGlobals++;
      return true;
    }
  } else if (LoadInst *L = dyn_cast<LoadInst>(Addr)) {
    if (isVtableAccess(L)) {
      // Reads from a vtable pointer can not race with any writes.
      NumOmittedReadsFromVtable++;
      return true;
    }
  }
  return false;
}

Value *InstrumentParallel::getPointerOperand(Value *I) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->getPointerOperand();
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->getPointerOperand();
  return nullptr;
}

bool InstrumentParallel::isNotInstrumentable(MDNode *mdNode) {

  DISubprogram *subProg = dyn_cast_or_null<llvm::DISubprogram>(mdNode);
  if(subProg) {
    if(subProg->getName().startswith(".omp") ||
       subProg->getLinkageName().startswith(".omp"))
      return true;
    else
      return false;
  }

  DILexicalBlockFile *subBlockFile = dyn_cast_or_null<llvm::DILexicalBlockFile>(mdNode);
  if(subBlockFile)
    return isNotInstrumentable(subBlockFile->getScope());

  DILexicalBlock *subBlock = dyn_cast_or_null<llvm::DILexicalBlock>(mdNode);
  if(subBlock)
    return isNotInstrumentable(subBlock->getScope());

  return false;
}

// Instrumenting some of the accesses may be proven redundant.
// Currently handled:
//  - read-before-write (within same BB, no calls between)
//  - not captured variables
//
// We do not handle some of the patterns that should not survive
// after the classic compiler optimizations.
// E.g. two reads from the same temp should be eliminated by CSE,
// two writes should be eliminated by DSE, etc.
//
// 'Local' is a vector of insns within the same BB (no calls between).
// 'All' is a vector of insns that will be instrumented.
void InstrumentParallel::chooseInstructionsToInstrument(
    SmallVectorImpl<Instruction *> &Local, SmallVectorImpl<Instruction *> &All,
    const DataLayout &DL) {
  std::vector<Instruction*> LoadsAndStoresTotal;
  std::vector<Instruction*> LoadsAndStoresToRemove;
  SmallSet<Value*, 8> WriteTargets;
  // Iterate from the end.
  for (Instruction *I : reverse(Local)) {
    if(I->getMetadata("sword.ompstatus"))
      continue;
    // BAD!: This will remove any instrumentation basically
    // if(DebugLoc debugLoc = I->getDebugLoc()) {
    //   if(MDNode *mdNode = debugLoc.getScope()) {
    //     if(isNotInstrumentable(mdNode))
    //       continue;
    //   }
    // }
    if (StoreInst *Store = dyn_cast<StoreInst>(I)) {
      Value *Addr = Store->getPointerOperand();
      if (!shouldInstrumentReadWriteFromAddress(Addr))
        continue;
      WriteTargets.insert(Addr);
    } else {
      LoadInst *Load = cast<LoadInst>(I);
      Value *Addr = Load->getPointerOperand();
      if (!shouldInstrumentReadWriteFromAddress(Addr))
        continue;
      if (WriteTargets.count(Addr)) {
        // We will write to this temp, so no reason to analyze the read.
        NumOmittedReadsBeforeWrite++;
        continue;
      }
      if (addrPointsToConstantData(Addr)) {
        // Addr points to some constant data -- it can not race with any writes.
        continue;
      }
    }
    Value *Addr = isa<StoreInst>(*I)
        ? cast<StoreInst>(I)->getPointerOperand()
        : cast<LoadInst>(I)->getPointerOperand();
    if (isa<AllocaInst>(GetUnderlyingObject(Addr, DL)) &&
        !PointerMayBeCaptured(Addr, true, true)) {
      // The variable is addressable but not captured, so it cannot be
      // referenced from a different thread and participate in a data race
      // (see llvm/Analysis/CaptureTracking.h for details).
      NumOmittedNonCaptured++;
      continue;
    }
    All.push_back(I);
  }

  for(SmallVectorImpl<Instruction *>::iterator it=All.begin() ; it < All.end(); it++) {
    for(unsigned i = 0; i < (*it)->getNumOperands(); i++) {
      if(((*it)->getOperand(i)->getName().compare(".omp.iv") == 0) ||
         ((*it)->getOperand(i)->getName().compare(".omp.lb") == 0) ||
         ((*it)->getOperand(i)->getName().compare(".omp.ub") == 0) ||
         ((*it)->getOperand(i)->getName().compare(".omp.stride") == 0) ||
         ((*it)->getOperand(i)->getName().compare(".omp.is_last") == 0) ||
         ((*it)->getOperand(i)->getName().compare(".global_tid") == 0)) {
        All.erase(it);
        break;
      }
    }
  }

  /*
  // AliasAnalysis
  AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  for (Instruction *I1 : LoadsAndStoresTotal) {
    for (Instruction *I2 : LoadsAndStoresTotal) {
      if(I1 != I2) {
        if(!((AA.alias(MemoryLocation::get(I1), MemoryLocation::get(I2)) == MayAlias) ||
             (AA.alias(MemoryLocation::get(I1), MemoryLocation::get(I2)) == MustAlias))) {
          LoadsAndStoresToRemove.push_back(I1);
          // dbgs() << "No alias: "<< *I1 << "\n";
        }
      }
    }
  }

  bool Found;
  for(Instruction *I1 : LoadsAndStoresTotal) {
    // dbgs() << "Restart\n";
    Found = false;
    for(Instruction *I2 : LoadsAndStoresToRemove) {
      if(I1 == I2) {
        // dbgs() << "Break: " << *I1 << "\n";
        Found = true;
      }
      if(Found)
        break;
    }
    if(!Found) {
      // dbgs() << "Inserting: " << *I1 << "\n";
      All.push_back(I1);
    }
  }
  */
  Local.clear();
}

static bool isAtomic(Instruction *I) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->isAtomic() && LI->getSynchScope() == CrossThread;
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->isAtomic() && SI->getSynchScope() == CrossThread;
  if (isa<AtomicRMWInst>(I))
    return true;
  if (isa<AtomicCmpXchgInst>(I))
    return true;
  if (isa<FenceInst>(I))
    return true;
  return false;
}

void InstrumentParallel::setMetadata(Instruction *Inst, const char *name, const char *description) {
  LLVMContext& C = Inst->getContext();
  MDNode* N = MDNode::get(C, MDString::get(C, description));
  Inst->setMetadata(name, N);
}

bool InstrumentParallel::runOnFunction(Function &F) {
  if (&F == SwordCtorFunction)
  	return false;
  Function *IF;
  llvm::GlobalVariable *ompStatusGlobal = NULL;
  Module *M = F.getParent();
  StringRef functionName = F.getName();

  ConstantInt *Zero8 = ConstantInt::get(Type::getInt8Ty(M->getContext()), 0);
  ConstantInt *Zero32 = ConstantInt::get(Type::getInt32Ty(M->getContext()), 0);
  ConstantInt *Zero64 = ConstantInt::get(Type::getInt64Ty(M->getContext()), 0);
  ConstantInt *One = ConstantInt::get(Type::getInt32Ty(M->getContext()), 1);

  ompStatusGlobal = M->getNamedGlobal("__sword_status__");
  if(functionName.compare("main") == 0) {
    if(!ompStatusGlobal) {
      IntegerType *Int32Ty = IntegerType::getInt32Ty(M->getContext());
      ompStatusGlobal =
        new llvm::GlobalVariable(*M, Int32Ty, false,
                                 llvm::GlobalValue::CommonLinkage,
                                 Zero32, "__sword_status__", NULL,
                                 GlobalVariable::GeneralDynamicTLSModel,
                                 0, false);
    } else if(ompStatusGlobal &&
              (ompStatusGlobal->getLinkage() != llvm::GlobalValue::CommonLinkage)) {
      ompStatusGlobal->setLinkage(llvm::GlobalValue::CommonLinkage);
      ompStatusGlobal->setExternallyInitialized(false);
      ompStatusGlobal->setInitializer(Zero32);
    }

    IRBuilder<> IRB2(M->getContext());
    Constant* constant = M->getOrInsertFunction("__sword__get_omp_status",
    		IRB2.getInt32Ty(),
			NULL);
    Function* __sword_get_omp_status = cast<Function>(constant);
    __sword_get_omp_status->setCallingConv(CallingConv::C);
    BasicBlock* block2 = BasicBlock::Create(M->getContext(), "entry", __sword_get_omp_status);
    IRBuilder<> builder2(block2);
    LoadInst *loadOmpStatus = builder2.CreateLoad(IRB2.getInt32Ty(), ompStatusGlobal);
    builder2.CreateRet(loadOmpStatus);

    F.removeFnAttr(llvm::Attribute::SanitizeThread);

#ifndef LIBOMP_ANNOTATION_TSAN_SUPPORT
    // Add function for Tsan suppressions
    // const char *__tsan_default_suppressions() {
    //   return "called_from_lib:libomp.so\nthread:^__kmp_create_worker$\n";
    // }
    ArrayType* ArrayTy_0 = ArrayType::get(IntegerType::get(M->getContext(), 8), 57);
    GlobalVariable* suppression_str =
      new GlobalVariable(*M,
                         ArrayTy_0,
                         true,
                         GlobalValue::PrivateLinkage,
                         0,
                         "__tsan_default_suppressions_value");
    suppression_str->setAlignment(1);
    IRBuilder<> IRB(M->getContext());
    Constant* c = M->getOrInsertFunction("__tsan_default_suppressions",
                                         IRB.getInt8PtrTy(),
                                         NULL);
    Constant *suppression_str_const =
      ConstantDataArray::getString(M->getContext(),
      "called_from_lib:libomp.so\nthread:^__kmp_create_worker$\n", true);
    suppression_str->setInitializer(suppression_str_const);
    Function* __tsan_default_suppressions = cast<Function>(c);
    __tsan_default_suppressions->setCallingConv(CallingConv::C);
    BasicBlock* block = BasicBlock::Create(M->getContext(), "entry", __tsan_default_suppressions);
    IRBuilder<> builder(block);
    builder.CreateRet(suppression_str);
#endif
    return true;
  }

  if(functionName.endswith("_dtor") ||
     functionName.endswith("__sword__") ||
     functionName.endswith("__clang_call_terminate") ||
     functionName.endswith("__tsan_default_suppressions") ||
	 functionName.endswith("__sword__get_omp_status") ||
     (F.getLinkage() == llvm::GlobalValue::AvailableExternallyLinkage)) {
    return true;
  }

  if(!ompStatusGlobal) {
    IntegerType *Int32Ty = IntegerType::getInt32Ty(M->getContext());
    ompStatusGlobal =
      new llvm::GlobalVariable(*M, Int32Ty, false,
                               llvm::GlobalValue::AvailableExternallyLinkage,
                               0, "__sword_status__", NULL,
                               GlobalVariable::GeneralDynamicTLSModel,
                               0, true);
  }

  if(functionName.startswith(".omp")) {
    // Increment of __sword_status__
    Instruction *entryBBI = &F.getEntryBlock().front();
    LoadInst *loadInc = new LoadInst(ompStatusGlobal, "loadIncOmpStatus", false, entryBBI);
    loadInc->setAlignment(4);
    setMetadata(loadInc, "sword.ompstatus", "SWORD Instrumentation");
    Instruction *inc = BinaryOperator::Create (BinaryOperator::Add,
                                               loadInc, One,
                                               "incOmpStatus",
                                               entryBBI);
    setMetadata(loadInc, "sword.ompstatus", "sword Instrumentation");
    StoreInst *storeInc = new StoreInst(inc, ompStatusGlobal, entryBBI);
    storeInc->setAlignment(4);
    setMetadata(storeInc, "sword.ompstatus", "sword Instrumentation");

    // Decrement of __sword_status__
    Instruction *exitBBI = F.back().getTerminator();
    if(exitBBI) {
      LoadInst *loadDec = new LoadInst(ompStatusGlobal, "loadDecOmpStatus", false, exitBBI);
      loadDec->setAlignment(4);
      setMetadata(loadDec, "sword.ompstatus", "sword Instrumentation");
      Instruction *dec = BinaryOperator::Create (BinaryOperator::Sub,
                                                 loadDec, One,
                                                 "decOmpStatus",
                                                 exitBBI);
      StoreInst *storeDec = new StoreInst(dec, ompStatusGlobal, exitBBI);
      storeDec->setAlignment(4);
      setMetadata(storeDec, "sword.ompstatus", "sword Instrumentation");
    } else {
      report_fatal_error("Broken function found, compilation aborted!");
    }
    IF = &F;
  } else {
    ValueToValueMapTy VMap;
    Function *new_function = CloneFunction(&F, VMap);
    new_function->setName(functionName + "__sword__");
    Function::ArgumentListType::iterator it = F.getArgumentList().begin();
    Function::ArgumentListType::iterator end = F.getArgumentList().end();
    std::vector<Value*> args;
    while (it != end) {
      Argument *Args = &(*it);
      args.push_back(Args);
      it++;
    }

    // Removing SanitizeThread attribute so the sequential functions
    // won't be instrumented
    F.removeFnAttr(llvm::Attribute::SanitizeThread);

    // Instruction *firstEntryBBI = &F.getEntryBlock().front();
    Instruction *firstEntryBBI = NULL;
    Instruction *firstEntryBBDI = NULL;
    // for (auto &BB : F) {
    for (auto &Inst : F.getEntryBlock()) {
      if(Inst.getDebugLoc()) {
        firstEntryBBDI = &Inst;
        break;
      }
    }
    firstEntryBBI = &F.getEntryBlock().front();
    // }
    if(!firstEntryBBI || !firstEntryBBI)
      report_fatal_error("No instructions with debug information!");

    LoadInst *loadOmpStatus = new LoadInst(ompStatusGlobal, "loadOmpStatus", false, firstEntryBBI);
    Instruction *CondInst = new ICmpInst(firstEntryBBI, ICmpInst::ICMP_EQ, loadOmpStatus, One, "__sword__cond");

    BasicBlock *newEntryBB = F.getEntryBlock().splitBasicBlock(firstEntryBBI, "__sword__entry");
    F.getEntryBlock().back().eraseFromParent();
    BasicBlock *swordThenBB = BasicBlock::Create(M->getContext(), "__sword__if.then", &F);
    BranchInst::Create(swordThenBB, newEntryBB, CondInst, &F.getEntryBlock());

    // For now we assing the debug loc of the first instruction of the
    // cloned function
    if(new_function->getReturnType()->isVoidTy()) {
      CallInst *parallelCall = CallInst::Create(new_function, args, "", swordThenBB);
      parallelCall->setDebugLoc(firstEntryBBDI->getDebugLoc());
      ReturnInst::Create(M->getContext(), nullptr, swordThenBB);
    } else {
      CallInst *parallelCall = CallInst::Create(new_function, args, functionName + "__sword__", swordThenBB);
      parallelCall->setDebugLoc(firstEntryBBDI->getDebugLoc());
      ReturnInst::Create(M->getContext(), parallelCall, swordThenBB);
    }
    IF = new_function;
 }

  // Instrumentation
  initializeCallbacks(*IF->getParent());
  SmallVector<Instruction*, 8> RetVec;
  SmallVector<Instruction*, 8> AllLoadsAndStores;
  SmallVector<Instruction*, 8> LocalLoadsAndStores;
  SmallVector<Instruction*, 8> AtomicAccesses;
  SmallVector<Instruction*, 8> MemIntrinCalls;
  bool Res = false;
  bool HasCalls = false;
  const DataLayout &DL = IF->getParent()->getDataLayout();
  const TargetLibraryInfo *TLI =
      &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();

  // Traverse all instructions, collect loads/stores/returns, check for calls.
  for (auto &BB : *IF) {
    for (auto &Inst : BB) {
      if (isAtomic(&Inst))
        AtomicAccesses.push_back(&Inst);
      else if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst))
        LocalLoadsAndStores.push_back(&Inst);
      else if (isa<ReturnInst>(Inst))
        RetVec.push_back(&Inst);
      else if (isa<CallInst>(Inst) || isa<InvokeInst>(Inst)) {
        if (CallInst *CI = dyn_cast<CallInst>(&Inst))
          maybeMarkSanitizerLibraryCallNoBuiltin(CI, TLI);
        if (isa<MemIntrinsic>(Inst))
          MemIntrinCalls.push_back(&Inst);
        HasCalls = true;
        chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores,
                                       DL);
      }
    }
    chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores, DL);
  }

  // We have collected all loads and stores.
  // FIXME: many of these accesses do not need to be checked for races
  // (e.g. variables that do not escape, etc).

  // Instrument memory accesses only if we want to report bugs in the function.
  if (ClInstrumentMemoryAccesses)
    for (auto Inst : AllLoadsAndStores) {
      Res |= instrumentLoadOrStore(Inst, DL);
    }

  // Instrument atomic memory accesses in any case (they can be used to
  // implement synchronization).
  if (ClInstrumentAtomics)
    for (auto Inst : AtomicAccesses) {
      Res |= instrumentAtomic(Inst, DL);
    }

  // We do not instrument MemInstrinsics for now
  // if (ClInstrumentMemIntrinsics)
  //   for (auto Inst : MemIntrinCalls) {
  //     Res |= instrumentMemIntrinsic(Inst);
  //   }

  // Instrument function entry/exit points if there were instrumented accesses.
  if ((Res || HasCalls) && ClInstrumentFuncEntryExit) {
    IRBuilder<> IRB(IF->getEntryBlock().getFirstNonPHI());
    Value *ReturnAddress = IRB.CreateCall(
        Intrinsic::getDeclaration(IF->getParent(), Intrinsic::returnaddress),
        IRB.getInt32(0));
    IRB.CreateCall(SwordFuncEntry, ReturnAddress);
    for (auto RetInst : RetVec) {
      IRBuilder<> IRBRet(RetInst);
      IRBRet.CreateCall(SwordFuncExit);
    }
    Res = true;
  }

  return Res;
}

bool InstrumentParallel::instrumentLoadOrStore(Instruction *I,
                                            const DataLayout &DL) {
  IRBuilder<> IRB(I);
  bool IsWrite = isa<StoreInst>(*I);
  Value *Addr = IsWrite
      ? cast<StoreInst>(I)->getPointerOperand()
      : cast<LoadInst>(I)->getPointerOperand();
  int Idx = getMemoryAccessFuncIndex(Addr, DL);
  if (Idx < 0)
    return false;
  // if (IsWrite && isVtableAccess(I)) {
  //   DEBUG(dbgs() << "  VPTR : " << *I << "\n");
  //   Value *StoredValue = cast<StoreInst>(I)->getValueOperand();
  //   // StoredValue may be a vector type if we are storing several vptrs at once.
  //   // In this case, just take the first element of the vector since this is
  //   // enough to find vptr races.
  //   if (isa<VectorType>(StoredValue->getType()))
  //     StoredValue = IRB.CreateExtractElement(
  //         StoredValue, ConstantInt::get(IRB.getInt32Ty(), 0));
  //   if (StoredValue->getType()->isIntegerTy())
  //     StoredValue = IRB.CreateIntToPtr(StoredValue, IRB.getInt8PtrTy());
  //   // Call SwordVptrUpdate.
  //   IRB.CreateCall(SwordVptrUpdate,
  //                  {IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
  //                   IRB.CreatePointerCast(StoredValue, IRB.getInt8PtrTy())});
  //   NumInstrumentedVtableWrites++;
  //   return true;
  // }
  // if (!IsWrite && isVtableAccess(I)) {
  //   IRB.CreateCall(SwordVptrLoad,
  //                  IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()));
  //   NumInstrumentedVtableReads++;
  //   return true;
  // }
  const unsigned Alignment = IsWrite
      ? cast<StoreInst>(I)->getAlignment()
      : cast<LoadInst>(I)->getAlignment();
  Type *OrigTy = cast<PointerType>(Addr->getType())->getElementType();
  const uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  Value *OnAccessFunc = nullptr;
  if (Alignment == 0 || Alignment >= 8 || (Alignment % (TypeSize / 8)) == 0)
    OnAccessFunc = IsWrite ? SwordWrite[Idx] : SwordRead[Idx];
  else
    OnAccessFunc = IsWrite ? SwordUnalignedWrite[Idx] : SwordUnalignedRead[Idx];
   IRB.CreateCall(OnAccessFunc, IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()));
  if (IsWrite) NumInstrumentedWrites++;
  else         NumInstrumentedReads++;
  return true;
}

static ConstantInt *createOrdering(IRBuilder<> *IRB, AtomicOrdering ord) {
  uint32_t v = 0;
  switch (ord) {
    case AtomicOrdering::NotAtomic:
      llvm_unreachable("unexpected atomic ordering!");
    case AtomicOrdering::Unordered:              // Fall-through.
    case AtomicOrdering::Monotonic:              v = 0; break;
    // Not specified yet:
    // case AtomicOrdering::Consume:                v = 1; break;
    case AtomicOrdering::Acquire:                v = 2; break;
    case AtomicOrdering::Release:                v = 3; break;
    case AtomicOrdering::AcquireRelease:         v = 4; break;
    case AtomicOrdering::SequentiallyConsistent: v = 5; break;
  }
  return IRB->getInt32(v);
}

// If a memset intrinsic gets inlined by the code gen, we will miss races on it.
// So, we either need to ensure the intrinsic is not inlined, or instrument it.
// We do not instrument memset/memmove/memcpy intrinsics (too complicated),
// instead we simply replace them with regular function calls, which are then
// intercepted by the run-time.
// Since tsan is running after everyone else, the calls should not be
// replaced back with intrinsics. If that becomes wrong at some point,
// we will need to call e.g. __tsan_memset to avoid the intrinsics.
bool InstrumentParallel::instrumentMemIntrinsic(Instruction *I) {
  IRBuilder<> IRB(I);
  if (MemSetInst *M = dyn_cast<MemSetInst>(I)) {
    IRB.CreateCall(
        MemsetFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(1), IRB.getInt32Ty(), false),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
  } else if (MemTransferInst *M = dyn_cast<MemTransferInst>(I)) {
    IRB.CreateCall(
        isa<MemCpyInst>(M) ? MemcpyFn : MemmoveFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreatePointerCast(M->getArgOperand(1), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
  }
  return false;
}

static Value *createIntOrPtrToIntCast(Value *V, Type* Ty, IRBuilder<> &IRB) {
  return isa<PointerType>(V->getType()) ?
    IRB.CreatePtrToInt(V, Ty) : IRB.CreateIntCast(V, Ty, false);
}

// Both llvm and ThreadSanitizer atomic operations are based on C++11/C1x
// standards.  For background see C++11 standard.  A slightly older, publicly
// available draft of the standard (not entirely up-to-date, but close enough
// for casual browsing) is available here:
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2011/n3242.pdf
// The following page contains more background information:
// http://www.hpl.hp.com/personal/Hans_Boehm/c++mm/

bool InstrumentParallel::instrumentAtomic(Instruction *I, const DataLayout &DL) {
  IRBuilder<> IRB(I);
  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    Value *Addr = LI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     createOrdering(&IRB, LI->getOrdering())};
    Type *OrigTy = cast<PointerType>(Addr->getType())->getElementType();
    if (Ty == OrigTy) {
      Instruction *C = CallInst::Create(SwordAtomicLoad[Idx], Args);
      ReplaceInstWithInst(I, C);
    } else {
      // We are loading a pointer, so we need to cast the return value.
      Value *C = IRB.CreateCall(SwordAtomicLoad[Idx], Args);
      Instruction *Cast = CastInst::Create(Instruction::IntToPtr, C, OrigTy);
      ReplaceInstWithInst(I, Cast);
    }
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    Value *Addr = SI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     createIntOrPtrToIntCast(SI->getValueOperand(), Ty, IRB),
                     createOrdering(&IRB, SI->getOrdering())};
    CallInst *C = CallInst::Create(SwordAtomicStore[Idx], Args);
    ReplaceInstWithInst(I, C);
  } else if (AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I)) {
    Value *Addr = RMWI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    Function *F = SwordAtomicRMW[RMWI->getOperation()][Idx];
    if (!F)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     IRB.CreateIntCast(RMWI->getValOperand(), Ty, false),
                     createOrdering(&IRB, RMWI->getOrdering())};
    CallInst *C = CallInst::Create(F, Args);
    ReplaceInstWithInst(I, C);
  } else if (AtomicCmpXchgInst *CASI = dyn_cast<AtomicCmpXchgInst>(I)) {
    Value *Addr = CASI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *CmpOperand =
      createIntOrPtrToIntCast(CASI->getCompareOperand(), Ty, IRB);
    Value *NewOperand =
      createIntOrPtrToIntCast(CASI->getNewValOperand(), Ty, IRB);
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     CmpOperand,
                     NewOperand,
                     createOrdering(&IRB, CASI->getSuccessOrdering()),
                     createOrdering(&IRB, CASI->getFailureOrdering())};
    CallInst *C = IRB.CreateCall(SwordAtomicCAS[Idx], Args);
    Value *Success = IRB.CreateICmpEQ(C, CmpOperand);
    Value *OldVal = C;
    Type *OrigOldValTy = CASI->getNewValOperand()->getType();
    if (Ty != OrigOldValTy) {
      // The value is a pointer, so we need to cast the return value.
      OldVal = IRB.CreateIntToPtr(C, OrigOldValTy);
    }

    Value *Res =
      IRB.CreateInsertValue(UndefValue::get(CASI->getType()), OldVal, 0);
    Res = IRB.CreateInsertValue(Res, Success, 1);

    I->replaceAllUsesWith(Res);
    I->eraseFromParent();
  } else if (FenceInst *FI = dyn_cast<FenceInst>(I)) {
    Value *Args[] = {createOrdering(&IRB, FI->getOrdering())};
    Function *F = FI->getSynchScope() == SingleThread ?
        SwordAtomicSignalFence : SwordAtomicThreadFence;
    CallInst *C = CallInst::Create(F, Args);
    ReplaceInstWithInst(I, C);
  }
  return true;
}

int InstrumentParallel::getMemoryAccessFuncIndex(Value *Addr,
                                              const DataLayout &DL) {
  Type *OrigPtrTy = Addr->getType();
  Type *OrigTy = cast<PointerType>(OrigPtrTy)->getElementType();
  assert(OrigTy->isSized());
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize != 8  && TypeSize != 16 &&
      TypeSize != 32 && TypeSize != 64 && TypeSize != 128) {
    NumAccessesWithBadSize++;
    // Ignore all unusual sizes.
    return -1;
  }
  size_t Idx = countTrailingZeros(TypeSize / 8);
  assert(Idx < kNumberOfAccessSizes);
  return Idx;
}

static void registerInstrumentParallelPass(const PassManagerBuilder &, llvm::legacy::PassManagerBase &PM) {
  PM.add(new InstrumentParallel());
}

static RegisterStandardPasses RegisterMyPass(PassManagerBuilder::EP_EarlyAsPossible, registerInstrumentParallelPass);
