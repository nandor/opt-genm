// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <sstream>
#include <queue>

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/OptimizationRemarkEmitter.h>
#include <llvm/CodeGen/MachineFrameInfo.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/MachineJumpTableInfo.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/SelectionDAGISel.h>
#include <llvm/CodeGen/TargetFrameLowering.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Mangler.h>

#include "emitter/isel.h"
#include "core/block.h"
#include "core/cast.h"
#include "core/cfg.h"
#include "core/data.h"
#include "core/extern.h"
#include "core/func.h"
#include "core/inst.h"
#include "core/insts.h"
#include "core/prog.h"

namespace ISD = llvm::ISD;
using BranchProbability = llvm::BranchProbability;



// -----------------------------------------------------------------------------
BranchProbability kLikely = BranchProbability::getBranchProbability(99, 100);
BranchProbability kUnlikely = BranchProbability::getBranchProbability(1, 100);


// -----------------------------------------------------------------------------
static bool CompatibleType(Type at, Type it)
{
  if (it == at) {
    return true;
  }
  if (it == Type::V64 || at == Type::V64) {
    return at == Type::I64 || it == Type::I64;
  }
  return false;
}

// -----------------------------------------------------------------------------
static bool UsedOutside(ConstRef<Inst> inst, const Block *block)
{
  std::queue<ConstRef<Inst>> q;
  q.push(inst);

  while (!q.empty()) {
    ConstRef<Inst> i = q.front();
    q.pop();
    for (const Use &use : i->uses()) {
      // The use must be for the specific index.
      if (use != i) {
        continue;
      }

      auto *userInst = cast<const Inst>(use.getUser());
      if (userInst->Is(Inst::Kind::PHI)) {
        return true;
      }
      if (auto *movInst = ::cast_or_null<const MovInst>(userInst)) {
        if (CompatibleType(movInst->GetType(), i->GetType(0))) {
          q.push(movInst);
          continue;
        }
      }
      if (userInst->getParent() != block) {
        return true;
      }
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
ISel::ISel(char &ID, const Prog *prog, llvm::TargetLibraryInfo *libInfo)
  : llvm::ModulePass(ID)
  , prog_(prog)
  , libInfo_(libInfo)
  , MBB_(nullptr)
{
}

// -----------------------------------------------------------------------------
llvm::StringRef ISel::getPassName() const
{
  return "LLIR to LLVM SelectionDAG";
}

// -----------------------------------------------------------------------------
void ISel::getAnalysisUsage(llvm::AnalysisUsage &AU) const
{
  AU.addRequired<llvm::MachineModuleInfoWrapperPass>();
  AU.addPreserved<llvm::MachineModuleInfoWrapperPass>();
}

// -----------------------------------------------------------------------------
bool ISel::runOnModule(llvm::Module &Module)
{
  M_ = &Module;
  PrepareGlobals();

  // Generate code for functions.
  for (const Func &func : *prog_) {
    // Save a pointer to the current function.
    func_ = &func;
    lva_ = nullptr;
    frameIndex_ = 0;
    stackIndices_.clear();

    // Create a new dummy empty Function.
    // The IR function simply returns void since it cannot be empty.
    F_ = M_->getFunction(func.getName());

    // Create a MachineFunction, attached to the dummy one.
    auto *MF = funcs_[&func];
    auto ORE = std::make_unique<llvm::OptimizationRemarkEmitter>(F_);
    MF->setAlignment(llvm::Align(func.GetAlignment()));
    Lower(*MF);

    // Get a reference to the underlying DAG.
    auto &dag = GetDAG();

    // Initialise the dag with info for this function.
    llvm::FunctionLoweringInfo FLI;
    dag.init(*MF, *ORE, this, libInfo_, nullptr, nullptr, nullptr);
    dag.setFunctionLoweringInfo(&FLI);

    // Traverse nodes, entry first.
    llvm::ReversePostOrderTraversal<const Func*> blockOrder(&func);

    // Flag indicating if the function has VASTART.
    bool hasVAStart = false;

    // Prepare PHIs and arguments.
    auto *RegInfo = &MF->getRegInfo();
    for (const Block *block : blockOrder) {
      // First block in reverse post-order is the entry block.
      llvm::MachineBasicBlock *MBB = FLI.MBB = blocks_[block];

      // Allocate registers for exported values and create PHI
      // instructions for all PHI nodes in the basic block.
      for (const auto &inst : *block) {
        if (inst.Is(Inst::Kind::PHI)) {
          if (inst.use_empty()) {
            continue;
          }
          // Create a machine PHI instruction for all PHIs. The order of
          // machine PHIs should match the order of PHIs in the block.
          auto &phi = static_cast<const PhiInst &>(inst);
          auto reg = AssignVReg(&phi);
          BuildMI(MBB, DL_, GetInstrInfo().get(llvm::TargetOpcode::PHI), reg);
        } else if (inst.Is(Inst::Kind::ARG)) {
          // If the arg is used outside of entry, export it.
          if (UsedOutside(&inst, &func.getEntryBlock())) {
            AssignVReg(&inst);
          }
        } else {
          // If the value is used outside of the defining block, export it.
          for (unsigned i = 0, n = inst.GetNumRets(); i < n; ++i) {
            ConstRef<Inst> ref(&inst, i);
            if (IsExported(ref)) {
              AssignVReg(ref);
            }
          }
        }

        if (inst.Is(Inst::Kind::VASTART)) {
          hasVAStart = true;
        }
      }
    }

    // Lower individual blocks.
    for (const Block *block : blockOrder) {
      MBB_ = blocks_[block];

      {
        // If this is the entry block, lower all arguments.
        if (block == &func.getEntryBlock()) {
          LowerArguments(hasVAStart);

          // Set the stack size of the new function.
          auto &MFI = MF->getFrameInfo();
          for (auto &object : func.objects()) {
            auto index = MFI.CreateStackObject(
                object.Size,
                llvm::Align(object.Alignment),
                false
            );
            stackIndices_.insert({ object.Index, index });
          }
        }

        // Define incoming registers to landing pads.
        if (block->IsLandingPad()) {
          assert(block->pred_size() == 1 && "landing pad with multiple preds");
          auto *pred = *block->pred_begin();
          auto *call = ::cast_or_null<const InvokeInst>(pred->GetTerminator());
          assert(call && "landing pat does not follow invoke");
        }

        // Set up the SelectionDAG for the block.
        for (const auto &inst : *block) {
          Lower(&inst);
        }
      }

      // Ensure all values were exported.
      assert(!HasPendingExports() && "not all values were exported");

      // Lower the block.
      insert_ = MBB_->end();
      CodeGenAndEmitDAG();

      // Assertion to ensure that frames follow calls.
      for (auto it = MBB_->rbegin(); it != MBB_->rend(); it++) {
        if (it->isGCRoot() || it->isGCCall()) {
          auto call = std::next(it);
          assert(call != MBB_->rend() && call->isCall() && "invalid frame");
        }
      }

      // Clear values, except exported ones.
      values_.clear();
    }

    // If the entry block has a predecessor, insert a dummy entry.
    llvm::MachineBasicBlock *entryMBB = blocks_[&func.getEntryBlock()];
    if (entryMBB->pred_size() != 0) {
      MBB_ = MF->CreateMachineBasicBlock();
      dag.setRoot(dag.getNode(
          ISD::BR,
          SDL_,
          MVT::Other,
          dag.getRoot(),
          dag.getBasicBlock(entryMBB)
      ));

      insert_ = MBB_->end();
      CodeGenAndEmitDAG();

      MF->push_front(MBB_);
      MBB_->addSuccessor(entryMBB);
      entryMBB = MBB_;
    }

    // Emit copies from args into vregs at the entry.
    const auto &TRI = *MF->getSubtarget().getRegisterInfo();
    RegInfo->EmitLiveInCopies(entryMBB, TRI, GetInstrInfo());

    GetTargetLowering().finalizeLowering(*MF);

    MF->verify(nullptr, "LLIR-to-X86 ISel");

    MBB_ = nullptr;
    MF = nullptr;
  }

  // Finalize lowering of references.
  for (const auto &data : prog_->data()) {
    for (const Object &object : data) {
      for (const Atom &atom : object) {
        for (const Item &item : atom) {
          if (item.GetKind() != Item::Kind::EXPR) {
            continue;
          }

          auto *expr = item.GetExpr();
          switch (expr->GetKind()) {
            case Expr::Kind::SYMBOL_OFFSET: {
              auto *offsetExpr = static_cast<SymbolOffsetExpr *>(expr);
              if (auto *block = ::cast_or_null<Block>(offsetExpr->GetSymbol())) {
                auto *MBB = blocks_[block];
                auto *BB = const_cast<llvm::BasicBlock *>(MBB->getBasicBlock());

                MBB->setHasAddressTaken();
                llvm::BlockAddress::get(BB->getParent(), BB);
              }
              break;
            }
          }
        }
      }
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
void ISel::Lower(const Inst *i)
{
  if (i->IsTerminator()) {
    HandleSuccessorPHI(i->getParent());
  }

  switch (i->GetKind()) {
    // Nodes handled separately.
    case Inst::Kind::PHI:
    case Inst::Kind::ARG:
      return;
    // Target-specific instructions.
    case Inst::Kind::X86_XCHG:
    case Inst::Kind::X86_CMPXCHG:
    case Inst::Kind::X86_RDTSC:
    case Inst::Kind::X86_FNSTCW:
    case Inst::Kind::X86_FNSTSW:
    case Inst::Kind::X86_FNSTENV:
    case Inst::Kind::X86_FLDCW:
    case Inst::Kind::X86_FLDENV:
    case Inst::Kind::X86_LDMXCSR:
    case Inst::Kind::X86_STMXCSR:
    case Inst::Kind::X86_FNCLEX:
      return LowerArch(i);
    // Control flow.
    case Inst::Kind::CALL:     return LowerCall(static_cast<const CallInst *>(i));
    case Inst::Kind::TCALL:    return LowerTailCall(static_cast<const TailCallInst *>(i));
    case Inst::Kind::INVOKE:   return LowerInvoke(static_cast<const InvokeInst *>(i));
    case Inst::Kind::RET:      return LowerReturn(static_cast<const ReturnInst *>(i));
    case Inst::Kind::JCC:      return LowerJCC(static_cast<const JumpCondInst *>(i));
    case Inst::Kind::RAISE:    return LowerRaise(static_cast<const RaiseInst *>(i));
    case Inst::Kind::JMP:      return LowerJMP(static_cast<const JumpInst *>(i));
    case Inst::Kind::SWITCH:   return LowerSwitch(static_cast<const SwitchInst *>(i));
    case Inst::Kind::TRAP:     return LowerTrap(static_cast<const TrapInst *>(i));
    // Memory.
    case Inst::Kind::LD:       return LowerLD(static_cast<const LoadInst *>(i));
    case Inst::Kind::ST:       return LowerST(static_cast<const StoreInst *>(i));
    // Varargs.
    case Inst::Kind::VASTART:  return LowerVAStart(static_cast<const VAStartInst *>(i));
    // Constant.
    case Inst::Kind::FRAME:    return LowerFrame(static_cast<const FrameInst *>(i));
    // Dynamic stack allocation.
    case Inst::Kind::ALLOCA:   return LowerAlloca(static_cast<const AllocaInst *>(i));
    // Conditional.
    case Inst::Kind::SELECT:   return LowerSelect(static_cast<const SelectInst *>(i));
    // Unary instructions.
    case Inst::Kind::ABS:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FABS);
    case Inst::Kind::NEG:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FNEG);
    case Inst::Kind::SQRT:     return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FSQRT);
    case Inst::Kind::SIN:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FSIN);
    case Inst::Kind::COS:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FCOS);
    case Inst::Kind::SEXT:     return LowerSExt(static_cast<const SExtInst *>(i));
    case Inst::Kind::ZEXT:     return LowerZExt(static_cast<const ZExtInst *>(i));
    case Inst::Kind::XEXT:     return LowerXExt(static_cast<const XExtInst *>(i));
    case Inst::Kind::FEXT:     return LowerFExt(static_cast<const FExtInst *>(i));
    case Inst::Kind::MOV:      return LowerMov(static_cast<const MovInst *>(i));
    case Inst::Kind::TRUNC:    return LowerTrunc(static_cast<const TruncInst *>(i));
    case Inst::Kind::EXP:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FEXP);
    case Inst::Kind::EXP2:     return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FEXP2);
    case Inst::Kind::LOG:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FLOG);
    case Inst::Kind::LOG2:     return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FLOG2);
    case Inst::Kind::LOG10:    return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FLOG10);
    case Inst::Kind::FCEIL:    return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FCEIL);
    case Inst::Kind::FFLOOR:   return LowerUnary(static_cast<const UnaryInst *>(i), ISD::FFLOOR);
    case Inst::Kind::POPCNT:   return LowerUnary(static_cast<const UnaryInst *>(i), ISD::CTPOP);
    case Inst::Kind::CLZ:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::CTLZ);
    case Inst::Kind::CTZ:      return LowerUnary(static_cast<const UnaryInst *>(i), ISD::CTTZ);
    // Binary instructions.
    case Inst::Kind::CMP:      return LowerCmp(static_cast<const CmpInst *>(i));
    case Inst::Kind::UDIV:     return LowerBinary(i, ISD::UDIV, ISD::FDIV);
    case Inst::Kind::SDIV:     return LowerBinary(i, ISD::SDIV, ISD::FDIV);
    case Inst::Kind::UREM:     return LowerBinary(i, ISD::UREM, ISD::FREM);
    case Inst::Kind::SREM:     return LowerBinary(i, ISD::SREM, ISD::FREM);
    case Inst::Kind::MUL:      return LowerBinary(i, ISD::MUL,  ISD::FMUL);
    case Inst::Kind::ADD:      return LowerBinary(i, ISD::ADD,  ISD::FADD);
    case Inst::Kind::SUB:      return LowerBinary(i, ISD::SUB,  ISD::FSUB);
    case Inst::Kind::AND:      return LowerBinary(i, ISD::AND);
    case Inst::Kind::OR:       return LowerBinary(i, ISD::OR);
    case Inst::Kind::SLL:      return LowerBinary(i, ISD::SHL);
    case Inst::Kind::SRA:      return LowerBinary(i, ISD::SRA);
    case Inst::Kind::SRL:      return LowerBinary(i, ISD::SRL);
    case Inst::Kind::XOR:      return LowerBinary(i, ISD::XOR);
    case Inst::Kind::ROTL:     return LowerBinary(i, ISD::ROTL);
    case Inst::Kind::ROTR:     return LowerBinary(i, ISD::ROTR);
    case Inst::Kind::POW:      return LowerBinary(i, ISD::FPOW);
    case Inst::Kind::COPYSIGN: return LowerBinary(i, ISD::FCOPYSIGN);
    // Overflow checks.
    case Inst::Kind::UADDO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::UADDO);
    case Inst::Kind::UMULO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::UMULO);
    case Inst::Kind::USUBO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::USUBO);
    case Inst::Kind::SADDO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::SADDO);
    case Inst::Kind::SMULO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::SMULO);
    case Inst::Kind::SSUBO:    return LowerALUO(static_cast<const OverflowInst *>(i), ISD::SSUBO);
    // Undefined value.
    case Inst::Kind::UNDEF:    return LowerUndef(static_cast<const UndefInst *>(i));
    // Target-specific generics.
    case Inst::Kind::SET:      return LowerSet(static_cast<const SetInst *>(i));
    case Inst::Kind::SYSCALL:  return LowerSyscall(static_cast<const SyscallInst *>(i));
    case Inst::Kind::CLONE:    return LowerClone(static_cast<const CloneInst *>(i));
  }
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::GetValue(ConstRef<Inst> inst)
{
  if (auto vt = values_.find(inst); vt != values_.end()) {
    return vt->second;
  }

  if (auto rt = regs_.find(inst); rt != regs_.end()) {
    llvm::SelectionDAG &dag = GetDAG();
    return dag.getCopyFromReg(
        dag.getEntryNode(),
        SDL_,
        rt->second,
        GetVT(inst->GetType(0))
    );
  } else {
    return LowerConstant(inst);
  }
}

// -----------------------------------------------------------------------------
void ISel::Export(ConstRef<Inst> inst, SDValue value)
{
  values_[inst] = value;
  auto it = regs_.find(inst);
  if (it != regs_.end()) {
    if (inst.GetType() == Type::V64) {
      pendingValueInsts_.emplace(inst, it->second);
    } else {
      pendingPrimInsts_.emplace(inst, it->second);
    }
  }
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerGlobal(const Global &val, int64_t offset)
{
  if (offset == 0) {
    return LowerGlobal(val);
  } else {
    auto &DAG = GetDAG();
    return DAG.getNode(
        ISD::ADD,
        SDL_,
        GetPtrTy(),
        LowerGlobal(val),
        DAG.getConstant(offset, SDL_, GetPtrTy())
    );
  }
}

// -----------------------------------------------------------------------------
void ISel::LowerArgs(CallLowering &lowering)
{
  auto &DAG = GetDAG();
  auto &MF = DAG.getMachineFunction();

  for (auto &argLoc : lowering.args()) {
    SDValue arg;
    switch (argLoc.Kind) {
      case CallLowering::ArgLoc::Kind::REG: {
        unsigned Reg = MF.addLiveIn(argLoc.Reg, argLoc.RegClass);
        arg = DAG.getCopyFromReg(
            DAG.getEntryNode(),
            SDL_,
            Reg,
            argLoc.VT
        );
        break;
      }
      case CallLowering::ArgLoc::Kind::STK: {
        llvm::MachineFrameInfo &MFI = MF.getFrameInfo();
        int index = MFI.CreateFixedObject(argLoc.Size, argLoc.Idx, true);
        args_[argLoc.Index] = index;
        arg = DAG.getLoad(
            argLoc.VT,
            SDL_,
            DAG.getEntryNode(),
            DAG.getFrameIndex(index, GetPtrTy()),
            llvm::MachinePointerInfo::getFixedStack(
                MF,
                index
            )
        );
        break;
      }
    }

    for (const auto &block : *func_) {
      for (const auto &inst : block) {
        if (auto *argInst = ::cast_or_null<const ArgInst>(&inst)) {
          if (argInst->GetIdx() == argLoc.Index) {
            Export(argInst, arg);
          }
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::GetPrimitiveExportRoot()
{
  ExportList exports;
  for (auto &[reg, value] : pendingPrimValues_) {
    exports.emplace_back(reg, value);
  }
  for (auto &[inst, reg] : pendingPrimInsts_) {
    auto it = values_.find(inst);
    assert(it != values_.end() && "value not defined");
    exports.emplace_back(reg, it->second);
  }
  pendingPrimValues_.clear();
  pendingPrimInsts_.clear();
  return GetExportRoot(exports);
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::GetValueExportRoot()
{
  ExportList exports;
  for (auto &[inst, reg] : pendingValueInsts_) {
    auto it = values_.find(inst);
    assert(it != values_.end() && "value not defined");
    exports.emplace_back(reg, it->second);
  }
  pendingValueInsts_.clear();
  return GetExportRoot(exports);
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::GetExportRoot()
{
  ExportList exports;
  for (auto &[reg, value] : pendingPrimValues_) {
    exports.emplace_back(reg, value);
  }
  for (auto &[inst, reg] : pendingPrimInsts_) {
    auto it = values_.find(inst);
    assert(it != values_.end() && "value not defined");
    exports.emplace_back(reg, it->second);
  }
  for (auto &[inst, reg] : pendingValueInsts_) {
    auto it = values_.find(inst);
    assert(it != values_.end() && "value not defined");
    exports.emplace_back(reg, it->second);
  }
  pendingPrimValues_.clear();
  pendingPrimInsts_.clear();
  pendingValueInsts_.clear();
  return GetExportRoot(exports);
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::GetExportRoot(const ExportList &exports)
{
  llvm::SelectionDAG &dag = GetDAG();

  SDValue root = dag.getRoot();
  if (exports.empty()) {
    return root;
  }

  bool exportsRoot = false;
  llvm::SmallVector<llvm::SDValue, 8> chains;
  for (auto &exp : exports) {
    chains.push_back(dag.getCopyToReg(
        dag.getEntryNode(),
        SDL_,
        exp.first,
        exp.second
    ));

    auto *node = exp.second.getNode();
    if (node->getNumOperands() > 0 && node->getOperand(0) == root) {
      exportsRoot = true;
    }
  }

  if (root.getOpcode() != ISD::EntryToken && !exportsRoot) {
    chains.push_back(root);
  }

  SDValue factor = dag.getNode(
      ISD::TokenFactor,
      SDL_,
      MVT::Other,
      chains
  );
  dag.setRoot(factor);
  return factor;
}

// -----------------------------------------------------------------------------
bool ISel::HasPendingExports()
{
  if (!pendingPrimValues_.empty()) {
    return true;
  }
  if (!pendingPrimInsts_.empty()) {
    return true;
  }
  if (!pendingValueInsts_.empty()) {
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
unsigned ISel::AssignVReg(ConstRef<Inst> inst)
{
  MVT VT = GetVT(inst.GetType());

  auto *RegInfo = &GetDAG().getMachineFunction().getRegInfo();
  auto &tli = GetTargetLowering();
  auto reg = RegInfo->createVirtualRegister(tli.getRegClassFor(VT));
  regs_[inst] = reg;
  return reg;
}

// -----------------------------------------------------------------------------
void ISel::ExportValue(unsigned reg, llvm::SDValue value)
{
  pendingPrimValues_.emplace_back(reg, value);
}


// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerInlineAsm(
    const char *code,
    unsigned flags,
    llvm::ArrayRef<llvm::Register> inputs,
    llvm::ArrayRef<llvm::Register> clobbers,
    llvm::ArrayRef<llvm::Register> outputs,
    SDValue glue)
{
  auto &DAG = GetDAG();
  auto &MF = DAG.getMachineFunction();
  auto &RegInfo = MF.getRegInfo();
  auto &TLI = GetTargetLowering();

  // Set up the inline assembly node.
  llvm::SmallVector<SDValue, 7> ops;
  ops.push_back(DAG.getRoot());
  ops.push_back(DAG.getTargetExternalSymbol(
      code,
      TLI.getProgramPointerTy(DAG.getDataLayout())
  ));
  ops.push_back(DAG.getMDNode(nullptr));
  ops.push_back(DAG.getTargetConstant(
      flags,
      SDL_,
      TLI.getPointerTy(DAG.getDataLayout())
  ));

  // Find the flag for a register.
  auto GetFlag = [&](unsigned kind, llvm::Register reg) -> unsigned
  {
    if (llvm::Register::isVirtualRegister(reg)) {
      const auto *RC = RegInfo.getRegClass(reg);
      return llvm::InlineAsm::getFlagWordForRegClass(
          llvm::InlineAsm::getFlagWord(kind, 1),
          RC->getID()
      );
    } else {
      return llvm::InlineAsm::getFlagWord(kind, 1);
    }
  };

  // Register the output.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_RegDef, 1
    );
    for (llvm::Register reg : outputs) {
      unsigned flag = GetFlag(llvm::InlineAsm::Kind_RegDef, reg);
      ops.push_back(DAG.getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(DAG.getRegister(reg, MVT::i64));
    }
  }

  // Register the input.
  {
    for (llvm::Register reg : inputs) {
      unsigned flag = GetFlag(llvm::InlineAsm::Kind_RegUse, reg);
      ops.push_back(DAG.getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(DAG.getRegister(reg, MVT::i32));
    }
  }

  // Register clobbers.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_Clobber, 1
    );
    for (llvm::Register clobber : clobbers) {
      ops.push_back(DAG.getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(DAG.getRegister(clobber, MVT::i32));
    }
  }

  // Add the glue.
  if (glue) {
    ops.push_back(glue);
  }

  // Create the inlineasm node.
  return DAG.getNode(
      ISD::INLINEASM,
      SDL_,
      DAG.getVTList(MVT::Other, MVT::Glue),
      ops
  );
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerImm(const APInt &val, Type type)
{
  union U { int64_t i; double d; };
  switch (type) {
    case Type::I8:
      return GetDAG().getConstant(val.sextOrTrunc(8), SDL_, MVT::i8);
    case Type::I16:
      return GetDAG().getConstant(val.sextOrTrunc(16), SDL_, MVT::i16);
    case Type::I32:
      return GetDAG().getConstant(val.sextOrTrunc(32), SDL_, MVT::i32);
    case Type::I64:
    case Type::V64:
      return GetDAG().getConstant(val.sextOrTrunc(64), SDL_, MVT::i64);
    case Type::I128:
      return GetDAG().getConstant(val.sextOrTrunc(128), SDL_, MVT::i128);
    case Type::F32: {
      U u { .i = val.getSExtValue() };
      return GetDAG().getConstantFP(u.d, SDL_, MVT::f32);
    }
    case Type::F64: {
      U u { .i = val.getSExtValue() };
      return GetDAG().getConstantFP(u.d, SDL_, MVT::f64);
    }
    case Type::F80: {
      U u { .i = val.getSExtValue() };
      return GetDAG().getConstantFP(u.d, SDL_, MVT::f80);
    }
  }
  llvm_unreachable("invalid type");
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerImm(const APFloat &val, Type type)
{
  switch (type) {
    case Type::I8:
    case Type::I16:
    case Type::I32:
    case Type::I64:
    case Type::V64:
    case Type::I128:
      llvm_unreachable("not supported");
    case Type::F32:
      return GetDAG().getConstantFP(val, SDL_, MVT::f32);
    case Type::F64:
      return GetDAG().getConstantFP(val, SDL_, MVT::f64);
    case Type::F80:
      return GetDAG().getConstantFP(val, SDL_, MVT::f80);
  }
  llvm_unreachable("invalid type");
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerConstant(ConstRef<Inst> inst)
{
  if (ConstRef<MovInst> movInst = ::cast_or_null<MovInst>(inst)) {
    Type rt = movInst->GetType();
    switch (ConstRef<Value> val = GetMoveArg(movInst); val->GetKind()) {
      case Value::Kind::INST: {
        Error(inst.Get(), "not a constant");
      }
      case Value::Kind::CONST: {
        const Constant &constVal = *::cast_or_null<Constant>(val);
        switch (constVal.GetKind()) {
          case Constant::Kind::REG: {
            Error(inst.Get(), "not a constant");
          }
          case Constant::Kind::INT: {
            auto &constInst = static_cast<const ConstantInt &>(constVal);
            return LowerImm(constInst.GetValue(), rt);
          }
          case Constant::Kind::FLOAT: {
            auto &constFloat = static_cast<const ConstantFloat &>(constVal);
            return LowerImm(constFloat.GetValue(), rt);
          }
        }
        llvm_unreachable("invalid constant kind");
      }
      case Value::Kind::GLOBAL: {
        if (!IsPointerType(movInst->GetType())) {
          Error(movInst.Get(), "Invalid address type");
        }
        return LowerGlobal(*::cast_or_null<Global>(val), 0);
      }
      case Value::Kind::EXPR: {
        if (!IsPointerType(movInst->GetType())) {
          Error(movInst.Get(), "Invalid address type");
        }
        return LowerExpr(*::cast_or_null<Expr>(val));
      }
    }
    llvm_unreachable("invalid value kind");
  } else {
    Error(inst.Get(), "not a move instruction");
  }
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerExpr(const Expr &expr)
{
  switch (expr.GetKind()) {
    case Expr::Kind::SYMBOL_OFFSET: {
      auto &symOff = static_cast<const SymbolOffsetExpr &>(expr);
      return LowerGlobal(*symOff.GetSymbol(), symOff.GetOffset());
    }
  }
  llvm_unreachable("invalid expression");
}

// -----------------------------------------------------------------------------
ISD::CondCode ISel::GetCond(Cond cc)
{
  switch (cc) {
    case Cond::EQ:  return ISD::CondCode::SETEQ;
    case Cond::NE:  return ISD::CondCode::SETNE;
    case Cond::LE:  return ISD::CondCode::SETLE;
    case Cond::LT:  return ISD::CondCode::SETLT;
    case Cond::GE:  return ISD::CondCode::SETGE;
    case Cond::GT:  return ISD::CondCode::SETGT;
    case Cond::OEQ: return ISD::CondCode::SETOEQ;
    case Cond::ONE: return ISD::CondCode::SETONE;
    case Cond::OLE: return ISD::CondCode::SETOLE;
    case Cond::OLT: return ISD::CondCode::SETOLT;
    case Cond::OGE: return ISD::CondCode::SETOGE;
    case Cond::OGT: return ISD::CondCode::SETOGT;
    case Cond::UEQ: return ISD::CondCode::SETUEQ;
    case Cond::UNE: return ISD::CondCode::SETUNE;
    case Cond::ULE: return ISD::CondCode::SETULE;
    case Cond::ULT: return ISD::CondCode::SETULT;
    case Cond::UGE: return ISD::CondCode::SETUGE;
    case Cond::UGT: return ISD::CondCode::SETUGT;
  }
  llvm_unreachable("invalid condition");
}

// -----------------------------------------------------------------------------
ISel::FrameExports ISel::GetFrameExport(const Inst *frame)
{
  if (!lva_) {
    lva_.reset(new LiveVariables(func_));
  }

  auto &dag = GetDAG();
  auto &mf = dag.getMachineFunction();

  FrameExports exports;
  for (ConstRef<Inst> inst : lva_->LiveOut(frame)) {
    if (inst.GetType() != Type::V64) {
      continue;
    }
    if (inst.Get() == frame) {
      continue;
    }
    assert(inst->GetNumRets() == 1 && "invalid number of return values");

    // Constant values might be tagged as such, but are not GC roots.
    SDValue v = GetValue(inst);
    if (llvm::isa<llvm::GlobalAddressSDNode>(v)) {
      continue;
    }
    if (llvm::isa<llvm::ConstantSDNode>(v)) {
      continue;
    }
    exports.emplace_back(inst, v);
  }
  return exports;
}

// -----------------------------------------------------------------------------
llvm::SDValue ISel::LowerGCFrame(
    SDValue chain,
    SDValue glue,
    const Inst *inst,
    const uint32_t *mask,
    const llvm::ArrayRef<CallLowering::RetLoc> returns)
{
  auto &DAG = GetDAG();
  auto *MF = &DAG.getMachineFunction();
  auto &MMI = MF->getMMI();
  auto &TRI = GetRegisterInfo();
  const Func *func = inst->getParent()->getParent();

  if (func->GetCallingConv() == CallingConv::C) {
    // Generate a root frame with no liveness info in C methods.
    SDValue frameOps[] = { chain, glue };
    SDVTList frameTypes = DAG.getVTList(MVT::Other, MVT::Glue);
    auto *symbol = MMI.getContext().createTempSymbol();
    return DAG.getGCFrame(SDL_, ISD::ROOT, frameTypes, frameOps, symbol);
  } else {
    // Generate a frame with liveness info in OCaml methods.
    const auto *frame =  inst->template GetAnnot<CamlFrame>();

    // Allocate a reg mask which does not block the return register.
    uint32_t *frameMask = MF->allocateRegMask();
    unsigned maskSize = llvm::MachineOperand::getRegMaskSize(TRI.getNumRegs());
    memcpy(frameMask, mask, sizeof(frameMask[0]) * maskSize);

    for (auto &ret : returns) {
      // Create a mask with all regs but the return reg.
      llvm::Register r = ret.Reg;
      for (llvm::MCSubRegIterator SR(r, &TRI, true); SR.isValid(); ++SR) {
        frameMask[*SR / 32] |= 1u << (*SR % 32);
      }
    }

    llvm::SmallVector<SDValue, 8> frameOps;
    frameOps.push_back(chain);
    frameOps.push_back(DAG.getRegisterMask(frameMask));
    for (auto &[inst, val] : GetFrameExport(inst)) {
      frameOps.push_back(val);
    }
    frameOps.push_back(glue);
    auto *symbol = MMI.getContext().createTempSymbol();
    frames_[symbol] = frame;
    SDVTList frameTypes = DAG.getVTList(MVT::Other, MVT::Glue);
    return DAG.getGCFrame(SDL_, ISD::CALL, frameTypes, frameOps, symbol);
  }
}

// -----------------------------------------------------------------------------
ConstRef<Value> ISel::GetMoveArg(ConstRef<MovInst> inst)
{
  if (ConstRef<MovInst> arg = ::cast_or_null<MovInst>(inst->GetArg())) {
    if (!CompatibleType(arg->GetType(), inst->GetType())) {
      return arg;
    }
    return GetMoveArg(arg);
  }
  return inst->GetArg();
}

// -----------------------------------------------------------------------------
bool ISel::IsExported(ConstRef<Inst> inst)
{
  if (inst->use_empty()) {
    return false;
  }
  if (inst->Is(Inst::Kind::PHI)) {
    return true;
  }

  if (ConstRef<MovInst> movInst = ::cast_or_null<MovInst>(inst)) {
    ConstRef<Value> val = GetMoveArg(movInst);
    switch (val->GetKind()) {
      case Value::Kind::INST: {
        break;
      }
      case Value::Kind::CONST: {
        const Constant &constVal = *::cast_or_null<Constant>(val);
        switch (constVal.GetKind()) {
          case Constant::Kind::REG: {
            break;
          }
          case Constant::Kind::INT:
          case Constant::Kind::FLOAT: {
            return false;
          }
        }
        break;
      }
      case Value::Kind::GLOBAL:
      case Value::Kind::EXPR: {
        return false;
      }
    }
  }

  return UsedOutside(inst, inst->getParent());
}

// -----------------------------------------------------------------------------
std::pair<bool, llvm::CallingConv::ID>
ISel::GetCallingConv(const Func *caller, const CallSite *call)
{
  bool needsTrampoline = false;
  if (caller->GetCallingConv() == CallingConv::CAML) {
    switch (call->GetCallingConv()) {
      case CallingConv::C: {
        needsTrampoline = call->HasAnnot<CamlFrame>();
        break;
      }
      case CallingConv::SETJMP:
      case CallingConv::CAML:
      case CallingConv::CAML_ALLOC:
      case CallingConv::CAML_GC:
      case CallingConv::CAML_RAISE: {
        break;
      }
    }
  }

  // Find the register mask, based on the calling convention.
  llvm::CallingConv::ID cc;
  {
    using namespace llvm::CallingConv;
    if (needsTrampoline) {
      cc = LLIR_CAML_EXT;
    } else {
      switch (call->GetCallingConv()) {
        case CallingConv::C:          cc = C;               break;
        case CallingConv::CAML:       cc = LLIR_CAML;       break;
        case CallingConv::CAML_ALLOC: cc = LLIR_CAML_ALLOC; break;
        case CallingConv::CAML_GC:    cc = LLIR_CAML_GC;    break;
        case CallingConv::CAML_RAISE: cc = LLIR_CAML_RAISE; break;
        case CallingConv::SETJMP:     cc = LLIR_SETJMP;     break;
      }
    }
  }

  return { needsTrampoline, cc };
}

// -----------------------------------------------------------------------------
void ISel::PrepareGlobals()
{
  auto &MMI = getAnalysis<llvm::MachineModuleInfoWrapperPass>().getMMI();
  auto &Ctx = M_->getContext();

  voidTy_ = llvm::Type::getVoidTy(Ctx);
  i8PtrTy_ = llvm::Type::getInt1PtrTy (Ctx);
  funcTy_ = llvm::FunctionType::get(voidTy_, {});

  // Convert LLIR visibility to LLVM linkage and visibility.
  auto GetVisibility = [](Visibility vis)
  {
    GlobalValue::LinkageTypes linkage;
    GlobalValue::VisibilityTypes visibility;
    switch (vis) {
      case Visibility::LOCAL: {
        linkage = GlobalValue::InternalLinkage;
        visibility = GlobalValue::DefaultVisibility;
        break;
      }
      case Visibility::GLOBAL_DEFAULT: {
        linkage = GlobalValue::ExternalLinkage;
        visibility = GlobalValue::DefaultVisibility;
        break;
      }
      case Visibility::GLOBAL_HIDDEN: {
        linkage = GlobalValue::ExternalLinkage;
        visibility = GlobalValue::HiddenVisibility;
        break;
      }
      case Visibility::WEAK_DEFAULT: {
        linkage = GlobalValue::WeakAnyLinkage;
        visibility = GlobalValue::DefaultVisibility;
        break;
      }
      case Visibility::WEAK_HIDDEN: {
        linkage = GlobalValue::WeakAnyLinkage;
        visibility = GlobalValue::HiddenVisibility;
        break;
      }
    }
    return std::make_pair(linkage, visibility);
  };

  // Create function definitions for all functions.
  for (const Func &func : *prog_) {
    // Determine the LLVM linkage type.
    auto [linkage, visibility] = GetVisibility(func.GetVisibility());

    // Add a dummy function to the module.
    auto *F = llvm::Function::Create(funcTy_, linkage, 0, func.getName(), M_);
    F->setVisibility(visibility);

    // Set a dummy calling conv to emulate the set
    // of registers preserved by the callee.
    llvm::CallingConv::ID cc;
    switch (func.GetCallingConv()) {
      case CallingConv::C:          cc = llvm::CallingConv::C;               break;
      case CallingConv::CAML:       cc = llvm::CallingConv::LLIR_CAML;       break;
      case CallingConv::CAML_RAISE: cc = llvm::CallingConv::LLIR_CAML_RAISE; break;
      case CallingConv::SETJMP:     cc = llvm::CallingConv::LLIR_SETJMP;     break;
      case CallingConv::CAML_ALLOC: llvm_unreachable("cannot define caml_alloc");
      case CallingConv::CAML_GC:    llvm_unreachable("cannot define caml_");
    }

    F->setCallingConv(cc);
    llvm::BasicBlock* block = llvm::BasicBlock::Create(F->getContext(), "entry", F);
    llvm::IRBuilder<> builder(block);
    builder.CreateRetVoid();

    // Create MBBs for each block.
    auto *MF = &MMI.getOrCreateMachineFunction(*F);
    funcs_[&func] = MF;
    for (const Block &block : func) {
      // Create a skeleton basic block, with a jump to itself.
      llvm::BasicBlock *BB = llvm::BasicBlock::Create(
          M_->getContext(),
          block.getName(),
          F,
          nullptr
      );
      llvm::BranchInst::Create(BB, BB);

      // Create the basic block to be filled in by the instruction selector.
      llvm::MachineBasicBlock *MBB = MF->CreateMachineBasicBlock(BB);
      MBB->setHasAddressTaken();
      blocks_[&block] = MBB;
      MF->push_back(MBB);
    }
  }

  // Create objects for all atoms.
  for (const auto &data : prog_->data()) {
    for (const Object &object : data) {
      for (const Atom &atom : object) {
        // Determine the LLVM linkage type.
        auto [linkage, visibility] = GetVisibility(atom.GetVisibility());

        auto *GV = new llvm::GlobalVariable(
            *M_,
            i8PtrTy_,
            false,
            linkage,
            nullptr,
            atom.getName()
        );
        GV->setVisibility(visibility);
      }
    }
  }

  // Create function declarations for externals.
  for (const Extern &ext : prog_->externs()) {
    auto [linkage, visibility] = GetVisibility(ext.GetVisibility());
    llvm::GlobalObject *GV;
    if (ext.GetSection() == ".text") {
      auto C = M_->getOrInsertFunction(ext.getName(), funcTy_);
      GV = llvm::cast<llvm::Function>(C.getCallee());
    } else {
      GV = new llvm::GlobalVariable(
          *M_,
          i8PtrTy_,
          false,
          linkage,
          nullptr,
          ext.getName(),
          nullptr,
          llvm::GlobalVariable::NotThreadLocal,
          0,
          true
      );
    }
    GV->setVisibility(visibility);
  }
}

// -----------------------------------------------------------------------------
void ISel::HandleSuccessorPHI(const Block *block)
{
  llvm::SelectionDAG &dag = GetDAG();
  llvm::MachineRegisterInfo &regInfo = dag.getMachineFunction().getRegInfo();
  const llvm::TargetLowering &tli = GetTargetLowering();

  auto *blockMBB = blocks_[block];
  llvm::SmallPtrSet<llvm::MachineBasicBlock *, 4> handled;
  for (const Block *succBB : block->successors()) {
    llvm::MachineBasicBlock *succMBB = blocks_[succBB];
    if (!handled.insert(succMBB).second) {
      continue;
    }

    auto phiIt = succMBB->begin();
    for (const PhiInst &phi : succBB->phis()) {
      if (phi.use_empty()) {
        continue;
      }

      llvm::MachineInstrBuilder mPhi(dag.getMachineFunction(), phiIt++);
      ConstRef<Inst> inst = phi.GetValue(block);
      unsigned reg = 0;
      Type phiType = phi.GetType();
      MVT VT = GetVT(phiType);

      if (ConstRef<MovInst> movInst = ::cast_or_null<MovInst>(inst)) {
        ConstRef<Value> arg = GetMoveArg(movInst);
        switch (arg->GetKind()) {
          case Value::Kind::INST: {
            auto it = regs_.find(inst);
            if (it != regs_.end()) {
              reg = it->second;
            } else {
              reg = regInfo.createVirtualRegister(tli.getRegClassFor(VT));
              ExportValue(reg, LowerConstant(inst));
            }
            break;
          }
          case Value::Kind::GLOBAL: {
            if (!IsPointerType(phi.GetType())) {
              Error(&phi, "Invalid address type");
            }
            reg = regInfo.createVirtualRegister(tli.getRegClassFor(VT));
            ExportValue(reg, LowerGlobal(*::cast_or_null<Global>(arg), 0));
            break;
          }
          case Value::Kind::EXPR: {
            if (!IsPointerType(phi.GetType())) {
              Error(&phi, "Invalid address type");
            }
            reg = regInfo.createVirtualRegister(tli.getRegClassFor(VT));
            ExportValue(reg, LowerExpr(*::cast_or_null<Expr>(arg)));
            break;
          }
          case Value::Kind::CONST: {
            const Constant &constVal = *::cast_or_null<Constant>(arg);
            switch (constVal.GetKind()) {
              case Constant::Kind::INT: {
                SDValue value = LowerImm(
                    static_cast<const ConstantInt &>(constVal).GetValue(),
                    phiType
                );
                reg = regInfo.createVirtualRegister(tli.getRegClassFor(VT));
                ExportValue(reg, value);
                break;
              }
              case Constant::Kind::FLOAT: {
                SDValue value = LowerImm(
                    static_cast<const ConstantFloat &>(constVal).GetValue(),
                    phiType
                );
                reg = regInfo.createVirtualRegister(tli.getRegClassFor(VT));
                ExportValue(reg, value);
                break;
              }
              case Constant::Kind::REG: {
                auto it = regs_.find(inst);
                if (it != regs_.end()) {
                  reg = it->second;
                } else {
                  Error(&phi, "Invalid incoming register to PHI.");
                }
                break;
              }
            }
            break;
          }
        }
      } else {
        auto it = regs_.find(inst);
        assert(it != regs_.end() && "missing vreg value");
        reg = it->second;
      }

      mPhi.addReg(reg).addMBB(blockMBB);
    }
  }
}

// -----------------------------------------------------------------------------
void ISel::CodeGenAndEmitDAG()
{
  bool changed;

  llvm::AAResults *aa = nullptr;
  llvm::SelectionDAG &DAG = GetDAG();
  llvm::CodeGenOpt::Level ol = GetOptLevel();

  DAG.NewNodesMustHaveLegalTypes = false;
  DAG.Combine(llvm::BeforeLegalizeTypes, aa, ol);
  changed = DAG.LegalizeTypes();
  DAG.NewNodesMustHaveLegalTypes = true;

  if (changed) {
    DAG.Combine(llvm::AfterLegalizeTypes, aa, ol);
  }

  changed = DAG.LegalizeVectors();

  if (changed) {
    DAG.LegalizeTypes();
    DAG.Combine(llvm::AfterLegalizeVectorOps, aa, ol);
  }

  DAG.Legalize();
  DAG.Combine(llvm::AfterLegalizeDAG, aa, ol);

  DoInstructionSelection();

  llvm::ScheduleDAGSDNodes *Scheduler = CreateScheduler();
  Scheduler->Run(&DAG, MBB_);

  llvm::MachineBasicBlock *Fst = MBB_;
  MBB_ = Scheduler->EmitSchedule(insert_);
  llvm::MachineBasicBlock *Snd = MBB_;

  if (Fst != Snd) {
    llvm_unreachable("not implemented");
  }
  delete Scheduler;

  DAG.clear();
}

// -----------------------------------------------------------------------------
class ISelUpdater : public llvm::SelectionDAG::DAGUpdateListener {
public:
  ISelUpdater(
      llvm::SelectionDAG &dag,
      llvm::SelectionDAG::allnodes_iterator &isp)
    : llvm::SelectionDAG::DAGUpdateListener(dag)
    , it_(isp)
  {
  }

  void NodeDeleted(llvm::SDNode *n, llvm::SDNode *) override {
    if (it_ == llvm::SelectionDAG::allnodes_iterator(n)) {
      ++it_;
    }
  }

private:
  llvm::SelectionDAG::allnodes_iterator &it_;
};

// -----------------------------------------------------------------------------
void ISel::DoInstructionSelection()
{
  llvm::SelectionDAG &dag = GetDAG();
  auto &TLI = GetTargetLowering();

  PreprocessISelDAG();

  dag.AssignTopologicalOrder();

  llvm::HandleSDNode dummy(dag.getRoot());
  llvm::SelectionDAG::allnodes_iterator it(dag.getRoot().getNode());
  ++it;

  ISelUpdater ISU(dag, it);

  while (it != dag.allnodes_begin()) {
    SDNode *node = &*--it;
    if (node->use_empty()) {
      continue;
    }

    if (!TLI.isStrictFPEnabled() && node->isStrictFPOpcode()) {
      EVT ActionVT;
      switch (node->getOpcode()) {
        case ISD::STRICT_SINT_TO_FP:
        case ISD::STRICT_UINT_TO_FP:
        case ISD::STRICT_LRINT:
        case ISD::STRICT_LLRINT:
        case ISD::STRICT_LROUND:
        case ISD::STRICT_LLROUND:
        case ISD::STRICT_FSETCC:
        case ISD::STRICT_FSETCCS: {
          ActionVT = node->getOperand(1).getValueType();
          break;
        }
        default: {
          ActionVT = node->getValueType(0);
          break;
        }
      }
      auto action = TLI.getOperationAction(node->getOpcode(), ActionVT);
      if (action == llvm::TargetLowering::Expand) {
        node = dag.mutateStrictFPToFP(node);
      }
    }
    Select(node);
  }

  dag.setRoot(dummy.getValue());

  PostprocessISelDAG();
}

// -----------------------------------------------------------------------------
[[noreturn]] void ISel::Error(const Inst *i, const std::string_view &message)
{
  auto block = i->getParent();
  auto func = block->getParent();

  std::ostringstream os;
  os << func->GetName() << "," << block->GetName() << ": " << message;
  llvm::report_fatal_error(os.str());
}

// -----------------------------------------------------------------------------
[[noreturn]] void ISel::Error(const Func *f, const std::string_view &message)
{
  std::ostringstream os;
  os << f->GetName() << ": " << message;
  llvm::report_fatal_error(os.str());
}

// -----------------------------------------------------------------------------
void ISel::LowerCall(const CallInst *inst)
{
  auto &dag = GetDAG();

  // Find the continuation block.
  auto *sourceMBB = blocks_[inst->getParent()];
  auto *contMBB = blocks_[inst->GetCont()];

  // Lower the call.
  LowerCallSite(dag.getRoot(), inst);

  // Add a jump to the continuation block.
  dag.setRoot(dag.getNode(
      ISD::BR,
      SDL_,
      MVT::Other,
      GetExportRoot(),
      dag.getBasicBlock(contMBB)
  ));

  // Mark successors.
  sourceMBB->addSuccessor(contMBB, BranchProbability::getOne());
}

// -----------------------------------------------------------------------------
void ISel::LowerTailCall(const TailCallInst *inst)
{
  LowerCallSite(GetDAG().getRoot(), inst);
}

// -----------------------------------------------------------------------------
void ISel::LowerInvoke(const InvokeInst *inst)
{
  auto &dag = GetDAG();

  // Find the continuation blocks.
  auto &MMI = dag.getMachineFunction().getMMI();
  auto *bCont = inst->GetCont();
  auto *bThrow = inst->GetThrow();
  auto *mbbCont = blocks_[bCont];
  auto *mbbThrow = blocks_[bThrow];

  // Mark the landing pad as such.
  mbbThrow->setIsEHPad();

  // Lower the invoke call: export here since the call might not return.
  LowerCallSite(GetPrimitiveExportRoot(), inst);

  // Add a jump to the continuation block: export the invoke result.
  dag.setRoot(dag.getNode(
      ISD::BR,
      SDL_,
      MVT::Other,
      GetExportRoot(),
      dag.getBasicBlock(mbbCont)
  ));

  // Mark successors.
  auto *sourceMBB = blocks_[inst->getParent()];
  sourceMBB->addSuccessor(mbbCont, BranchProbability::getOne());
  sourceMBB->addSuccessor(mbbThrow, BranchProbability::getZero());
  sourceMBB->normalizeSuccProbs();
}

// -----------------------------------------------------------------------------
void ISel::LowerBinary(const Inst *inst, unsigned op)
{
  auto *binaryInst = static_cast<const BinaryInst *>(inst);

  MVT type = GetVT(binaryInst->GetType());
  SDValue lhs = GetValue(binaryInst->GetLHS());
  SDValue rhs = GetValue(binaryInst->GetRHS());
  SDValue binary = GetDAG().getNode(op, SDL_, type, lhs, rhs);
  Export(inst, binary);
}

// -----------------------------------------------------------------------------
void ISel::LowerBinary(const Inst *inst, unsigned iop, unsigned fop)
{
  auto *binaryInst = static_cast<const BinaryInst *>(inst);
  switch (binaryInst->GetType()) {
    case Type::I8:
    case Type::I16:
    case Type::I32:
    case Type::I64:
    case Type::V64:
    case Type::I128: {
      LowerBinary(inst, iop);
      break;
    }
    case Type::F32:
    case Type::F64:
    case Type::F80: {
      LowerBinary(inst, fop);
      break;
    }
  }
}

// -----------------------------------------------------------------------------
void ISel::LowerUnary(const UnaryInst *inst, unsigned op)
{
  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();

  SDValue arg = GetValue(inst->GetArg());
  SDValue unary = GetDAG().getNode(op, SDL_, GetVT(retTy), arg);
  Export(inst, unary);
}

// -----------------------------------------------------------------------------
void ISel::LowerJCC(const JumpCondInst *inst)
{
  llvm::SelectionDAG &DAG = GetDAG();

  auto *sourceMBB = blocks_[inst->getParent()];
  auto *trueMBB = blocks_[inst->GetTrueTarget()];
  auto *falseMBB = blocks_[inst->GetFalseTarget()];

  ConstRef<Inst> condInst = inst->GetCond();

  if (trueMBB == falseMBB) {
    DAG.setRoot(DAG.getNode(
        ISD::BR,
        SDL_,
        MVT::Other,
        GetExportRoot(),
        DAG.getBasicBlock(trueMBB)
    ));

    sourceMBB->addSuccessor(trueMBB);
  } else {
    SDValue chain = GetExportRoot();
    SDValue cond = GetValue(condInst);

    cond = DAG.getSetCC(
        SDL_,
        GetFlagTy(),
        cond,
        DAG.getConstant(0, SDL_, GetVT(condInst.GetType())),
        ISD::CondCode::SETNE
    );

    chain = DAG.getNode(
        ISD::BRCOND,
        SDL_,
        MVT::Other,
        chain,
        cond,
        DAG.getBasicBlock(blocks_[inst->GetTrueTarget()])
    );

    chain = DAG.getNode(
        ISD::BR,
        SDL_,
        MVT::Other,
        chain,
        DAG.getBasicBlock(blocks_[inst->GetFalseTarget()])
    );

    DAG.setRoot(chain);

    sourceMBB->addSuccessorWithoutProb(trueMBB);
    sourceMBB->addSuccessorWithoutProb(falseMBB);
  }
  sourceMBB->normalizeSuccProbs();
}

// -----------------------------------------------------------------------------
void ISel::LowerJMP(const JumpInst *inst)
{
  llvm::SelectionDAG &DAG = GetDAG();

  const Block *target = inst->GetTarget();
  auto *sourceMBB = blocks_[inst->getParent()];
  auto *targetMBB = blocks_[target];

  DAG.setRoot(DAG.getNode(
      ISD::BR,
      SDL_,
      MVT::Other,
      GetExportRoot(),
      DAG.getBasicBlock(targetMBB)
  ));

  sourceMBB->addSuccessor(targetMBB);
}

// -----------------------------------------------------------------------------
void ISel::LowerLD(const LoadInst *ld)
{
  llvm::SelectionDAG &dag = GetDAG();

  Type type = ld->GetType();

  SDValue l = dag.getLoad(
      GetVT(type),
      SDL_,
      dag.getRoot(),
      GetValue(ld->GetAddr()),
      llvm::MachinePointerInfo(static_cast<llvm::Value *>(nullptr)),
      GetAlignment(type),
      llvm::MachineMemOperand::MONone,
      llvm::AAMDNodes(),
      nullptr
  );

  dag.setRoot(l.getValue(1));
  Export(ld, l);
}

// -----------------------------------------------------------------------------
void ISel::LowerST(const StoreInst *st)
{
  llvm::SelectionDAG &DAG = GetDAG();
  ConstRef<Inst> val = st->GetVal();
  DAG.setRoot(DAG.getStore(
      DAG.getRoot(),
      SDL_,
      GetValue(val),
      GetValue(st->GetAddr()),
      llvm::MachinePointerInfo(0u),
      GetAlignment(val.GetType()),
      llvm::MachineMemOperand::MONone,
      llvm::AAMDNodes()
  ));
}

// -----------------------------------------------------------------------------
void ISel::LowerFrame(const FrameInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  if (auto It = stackIndices_.find(inst->GetObject()); It != stackIndices_.end()) {
    SDValue base = dag.getFrameIndex(It->second, MVT::i64);
    if (auto offest = inst->GetOffset()) {
      Export(inst, dag.getNode(
          ISD::ADD,
          SDL_,
          MVT::i64,
          base,
          dag.getConstant(offest, SDL_, MVT::i64)
      ));
    } else {
      Export(inst, base);
    }
    return;
  }
  Error(inst, "invalid frame index");
}

// -----------------------------------------------------------------------------
void ISel::LowerCmp(const CmpInst *cmpInst)
{
  llvm::SelectionDAG &dag = GetDAG();

  MVT type = GetVT(cmpInst->GetType());
  SDValue lhs = GetValue(cmpInst->GetLHS());
  SDValue rhs = GetValue(cmpInst->GetRHS());
  ISD::CondCode cc = GetCond(cmpInst->GetCC());
  SDValue flag = dag.getSetCC(SDL_, MVT::i8, lhs, rhs, cc);
  if (type != MVT::i8) {
    flag = dag.getZExtOrTrunc(flag, SDL_, type);
  }
  Export(cmpInst, flag);
}

// -----------------------------------------------------------------------------
void ISel::LowerTrap(const TrapInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();
  dag.setRoot(dag.getNode(ISD::TRAP, SDL_, MVT::Other, dag.getRoot()));
}

// -----------------------------------------------------------------------------
void ISel::LowerMov(const MovInst *inst)
{
  Type retType = inst->GetType();

  ConstRef<Value> val = GetMoveArg(inst);
  switch (val->GetKind()) {
    case Value::Kind::INST: {
      ConstRef<Inst> arg = ::cast_or_null<Inst>(val);
      Type argType = arg.GetType();
      if (CompatibleType(argType, retType)) {
        Export(inst, GetValue(arg));
      } else if (GetSize(argType) == GetSize(retType)) {
        Export(inst, GetDAG().getBitcast(GetVT(retType), GetValue(arg)));
      } else {
        Error(inst, "unsupported mov");
      }
      return;
    }
    case Value::Kind::CONST: {
      const Constant &constVal = *::cast_or_null<Constant>(val);
      switch (constVal.GetKind()) {
        case Constant::Kind::REG: {
          auto &constReg = static_cast<const ConstantReg &>(constVal);
          Export(inst, LoadReg(constReg.GetValue()));
          return;
        }
        case Constant::Kind::INT:
        case Constant::Kind::FLOAT: {
          return;
        }
      }
      llvm_unreachable("invalid constant kind");
    }
    case Value::Kind::GLOBAL:
    case Value::Kind::EXPR: {
      return;
    }
  }
  llvm_unreachable("invalid value kind");
}

// -----------------------------------------------------------------------------
void ISel::LowerSExt(const SExtInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();
  MVT retMVT = GetVT(retTy);
  SDValue arg = GetValue(inst->GetArg());

  if (IsIntegerType(argTy)) {
    unsigned opcode;
    if (IsIntegerType(retTy)) {
      opcode = ISD::SIGN_EXTEND;
    } else {
      opcode = ISD::SINT_TO_FP;
    }

    Export(inst, dag.getNode(opcode, SDL_, retMVT, arg));
  } else {
    if (IsIntegerType(retTy)) {
      Export(inst, dag.getNode(ISD::FP_TO_SINT, SDL_, retMVT, arg));
    } else {
      Error(inst, "invalid sext: float -> float");
    }
  }
}

// -----------------------------------------------------------------------------
void ISel::LowerZExt(const ZExtInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();
  MVT retMVT = GetVT(retTy);
  SDValue arg = GetValue(inst->GetArg());

  if (IsIntegerType(argTy)) {
    unsigned opcode;
    if (IsIntegerType(retTy)) {
      opcode = ISD::ZERO_EXTEND;
    } else {
      opcode = ISD::UINT_TO_FP;
    }

    Export(inst, dag.getNode(opcode, SDL_, retMVT, arg));
  } else {
    if (IsIntegerType(retTy)) {
      Export(inst, dag.getNode(ISD::FP_TO_UINT, SDL_, retMVT, arg));
    } else {
      Error(inst, "invalid zext: float -> float");
    }
  }
}
// -----------------------------------------------------------------------------
void ISel::LowerXExt(const XExtInst *inst)
{
  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();
  MVT retMVT = GetVT(retTy);
  SDValue arg = GetValue(inst->GetArg());

  if (IsIntegerType(argTy)) {
    if (IsIntegerType(retTy)) {
      Export(inst, GetDAG().getNode(ISD::ANY_EXTEND, SDL_, retMVT, arg));
    } else {
      Error(inst, "invalid xext to float");
    }
  } else {
    Error(inst, "invalid xext from float");
  }
}

// -----------------------------------------------------------------------------
void ISel::LowerFExt(const FExtInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();

  if (!IsFloatType(argTy) || !IsFloatType(retTy)) {
    Error(inst, "argument/return not a float");
  }
  if (GetSize(argTy) >= GetSize(retTy)) {
    Error(inst, "Cannot shrink argument");
  }

  SDValue arg = GetValue(inst->GetArg());
  SDValue fext = dag.getNode(ISD::FP_EXTEND, SDL_, GetVT(retTy), arg);
  Export(inst, fext);
}

// -----------------------------------------------------------------------------
void ISel::LowerTrunc(const TruncInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  Type argTy = inst->GetArg()->GetType(0);
  Type retTy = inst->GetType();

  MVT retMVT = GetVT(retTy);
  SDValue arg = GetValue(inst->GetArg());

  unsigned opcode;
  if (IsFloatType(retTy)) {
    if (IsIntegerType(argTy)) {
      Error(inst, "Cannot truncate int -> float");
    } else {
      if (argTy == retTy) {
        Export(inst, dag.getNode(ISD::FTRUNC, SDL_, retMVT, arg));
      } else {
        Export(inst, dag.getNode(
            ISD::FP_ROUND,
            SDL_,
            retMVT,
            arg,
            dag.getTargetConstant(0, SDL_, GetPtrTy())
        ));
      }
    }
  } else {
    if (IsIntegerType(argTy)) {
      Export(inst, dag.getNode(ISD::TRUNCATE, SDL_, retMVT, arg));
    } else {
      Export(inst, dag.getNode(ISD::FP_TO_SINT, SDL_, retMVT, arg));
    }
  }
}

// -----------------------------------------------------------------------------
void ISel::LowerAlloca(const AllocaInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();
  llvm::MachineFunction &mf = dag.getMachineFunction();

  // Get the inputs.
  unsigned Align = inst->GetAlign();
  SDValue Size = GetValue(inst->GetCount());
  EVT VT = GetVT(inst->GetType());
  SDValue Chain = dag.getRoot();

  // Create a chain for unique ordering.
  Chain = dag.getCALLSEQ_START(Chain, 0, 0, SDL_);

  const llvm::TargetLowering &TLI = dag.getTargetLoweringInfo();
  unsigned SPReg = TLI.getStackPointerRegisterToSaveRestore();
  assert(SPReg && "Cannot find stack pointer");

  SDValue SP = dag.getCopyFromReg(Chain, SDL_, SPReg, VT);
  Chain = SP.getValue(1);

  // Adjust the stack pointer.
  SDValue Result = dag.getNode(ISD::SUB, SDL_, VT, SP, Size);
  if (Align > mf.getSubtarget().getFrameLowering()->getStackAlignment()) {
    Result = dag.getNode(
        ISD::AND,
        SDL_,
        VT,
        Result,
        dag.getConstant(-(uint64_t)Align, SDL_, VT)
    );
  }
  Chain = dag.getCopyToReg(Chain, SDL_, SPReg, Result);

  Chain = dag.getCALLSEQ_END(
      Chain,
      dag.getIntPtrConstant(0, SDL_, true),
      dag.getIntPtrConstant(0, SDL_, true),
      SDValue(),
      SDL_
  );

  dag.setRoot(Chain);
  Export(inst, Result);

  mf.getFrameInfo().setHasVarSizedObjects(true);
}

// -----------------------------------------------------------------------------
void ISel::LowerSelect(const SelectInst *select)
{
  SDValue node = GetDAG().getNode(
      ISD::SELECT,
      SDL_,
      GetVT(select->GetType()),
      GetValue(select->GetCond()),
      GetValue(select->GetTrue()),
      GetValue(select->GetFalse())
  );
  Export(select, node);
}

// -----------------------------------------------------------------------------
void ISel::LowerUndef(const UndefInst *inst)
{
  Export(inst, GetDAG().getUNDEF(GetVT(inst->GetType())));
}

// -----------------------------------------------------------------------------
void ISel::LowerALUO(const OverflowInst *inst, unsigned op)
{
  llvm::SelectionDAG &dag = GetDAG();

  MVT retType = GetVT(inst->GetType(0));
  MVT type = GetVT(inst->GetLHS()->GetType(0));
  SDValue lhs = GetValue(inst->GetLHS());
  SDValue rhs = GetValue(inst->GetRHS());

  SDVTList types = dag.getVTList(type, MVT::i1);
  SDValue node = dag.getNode(op, SDL_, types, lhs, rhs);
  SDValue flag = dag.getZExtOrTrunc(node.getValue(1), SDL_, retType);

  Export(inst, flag);
}
