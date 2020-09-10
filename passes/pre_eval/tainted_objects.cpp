// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include "passes/pre_eval/tainted_objects.h"
#include "core/atom.h"
#include "core/block.h"
#include "core/cast.h"
#include "core/extern.h"
#include "core/func.h"
#include "core/insts.h"
#include "core/inst_visitor.h"
#include "core/object.h"
#include "core/prog.h"



// -----------------------------------------------------------------------------
static bool AlwaysCalled(const Inst *inst)
{
  for (const User *user : inst->users()) {
    auto *userValue = static_cast<const Value *>(user);
    if (auto *userInst = ::dyn_cast_or_null<const Inst>(userValue)) {
      switch (userInst->GetKind()) {
        case Inst::Kind::CALL: {
          auto &site = static_cast<const CallInst &>(*userInst);
          if (site.GetCallee() != inst) {
            return false;
          }
          continue;
        }
        case Inst::Kind::TCALL:
        case Inst::Kind::INVOKE:
        case Inst::Kind::TINVOKE: {
          auto &site = static_cast<const CallSite<TerminatorInst> &>(*userInst);
          if (site.GetCallee() != inst) {
            return false;
          }
          continue;
        }
        default: {
          return false;
        }
      }
    } else {
      return false;
    }
  }
  return true;
}

// -----------------------------------------------------------------------------
const std::unordered_map<std::string, bool> kCallbacks =
{
  #define SYSCALL(name, callback) { #name, callback },
  #include "core/syscalls.h"
  #undef SYSCALL
};

// -----------------------------------------------------------------------------
bool TaintedObjects::Tainted::Union(const Tainted &that)
{
  bool changed = false;
  changed |= objects_.Union(that.objects_);
  changed |= funcs_.Union(that.funcs_);
  changed |= blocks_.Union(that.blocks_);
  return changed;
}

// -----------------------------------------------------------------------------
bool TaintedObjects::Tainted::Add(ID<Object> object)
{
  return objects_.Insert(object);
}

// -----------------------------------------------------------------------------
bool TaintedObjects::Tainted::Add(ID<Func> func)
{
  return funcs_.Insert(func);
}

// -----------------------------------------------------------------------------
bool TaintedObjects::Tainted::Add(ID<Block> block)
{
  return blocks_.Insert(block);
}

// -----------------------------------------------------------------------------
TaintedObjects::TaintedObjects(Func &entry)
{
  Explore({}, entry);
  do {
    Propagate();
  } while (ExpandIndirect());
}

// -----------------------------------------------------------------------------
TaintedObjects::~TaintedObjects()
{
}

// -----------------------------------------------------------------------------
std::optional<TaintedObjects::Tainted> TaintedObjects::operator[](
    Block &block) const
{
  if (auto it = blockSites_.find(&*block.begin()); it != blockSites_.end()) {
    Tainted tainted;
    for (auto blockID : it->second) {
      tainted.Union(blocks_.Find(blockID)->Taint);
    }
    return tainted;
  }
  return std::nullopt;
}

// -----------------------------------------------------------------------------
class BlockBuilder final : public InstVisitor {
public:
  BlockBuilder(
      TaintedObjects *objs,
      ID<TaintedObjects::BlockInfo> id,
      TaintedObjects::BlockInfo *&info,
      const TaintedObjects::CallString<TaintedObjects::N> &cs)
    : objs_(objs)
    , id_(id)
    , info_(info)
    , cs_(cs)
  {}

  void VisitCall(CallInst *i) override
  {
    if (auto ret = VisitCall(*info_, *i)) {
      auto next = objs_->MapInst(cs_, &*std::next(i->getIterator()));
      ret->Successors.Insert(next);
      info_ = objs_->blocks_.Find(next);
    }
  }

  void VisitTailCall(TailCallInst *i) override
  {
    if (auto ret = VisitCall(*info_, *i)) {
      ret->Successors.Insert(objs_->MapFunc(cs_, i->getParent()->getParent()));
    }
  }

  void VisitInvoke(InvokeInst *i) override
  {
    if (auto ret = VisitCall(*info_, *i)) {
      ret->Successors.Insert(objs_->MapBlock(cs_, i->GetCont()));
      ret->Successors.Insert(objs_->MapBlock(cs_, i->GetThrow()));
    }
  }

  void VisitTailInvoke(TailInvokeInst *i) override
  {
    if (auto ret = VisitCall(*info_, *i)) {
      ret->Successors.Insert(objs_->MapBlock(cs_, i->GetThrow()));
      ret->Successors.Insert(objs_->MapFunc(cs_, i->getParent()->getParent()));
    }
  }

  void VisitReturn(ReturnInst *i) override
  {
    info_->Successors.Insert(objs_->MapFunc(cs_, i->getParent()->getParent()));
  }

  void VisitJumpCond(JumpCondInst *i) override
  {
    info_->Successors.Insert(objs_->MapBlock(cs_, i->GetTrueTarget()));
    info_->Successors.Insert(objs_->MapBlock(cs_, i->GetFalseTarget()));
  }

  void VisitJumpIndirect(JumpIndirectInst *i) override
  {
    objs_->indirectJumps_.emplace_back(cs_.Append(i), id_);
  }

  void VisitJump(JumpInst *i) override
  {
    info_->Successors.Insert(objs_->MapBlock(cs_, i->GetTarget()));
  }

  void VisitSwitch(SwitchInst *sw) override
  {
    for (unsigned i = 0, n = sw->getNumSuccessors(); i < n; ++i) {
      info_->Successors.Insert(objs_->MapBlock(cs_, sw->getSuccessor(i)));
    }
  }

  void VisitTrap(TrapInst *i) override
  {
  }

  void VisitMov(MovInst *inst) override
  {
    auto *arg = static_cast<const MovInst *>(inst)->GetArg();
    auto &taint = info_->Taint;
    switch (arg->GetKind()) {
      case Value::Kind::CONST:
      case Value::Kind::INST: {
        return;
      }
      case Value::Kind::GLOBAL: {
        switch (static_cast<Global *>(arg)->GetKind()) {
          case Global::Kind::EXTERN: {
            return;
          }
          case Global::Kind::BLOCK: {
            taint.Add(objs_->blockMap_.Map(static_cast<Block *>(arg)));
            return;
          }
          case Global::Kind::FUNC: {
            if (!AlwaysCalled(inst)) {
              taint.Add(objs_->funcMap_.Map(static_cast<Func *>(arg)));
            }
            return;
          }
          case Global::Kind::ATOM: {
            auto *obj = static_cast<Atom *>(arg)->getParent();
            taint.Add(objs_->objectMap_.Map(obj));
            return;
          }
        }
        llvm_unreachable("invalid global kind");
      }
      case Value::Kind::EXPR: {
        switch (static_cast<Expr *>(arg)->GetKind()) {
          case Expr::Kind::SYMBOL_OFFSET: {
            auto *sym = static_cast<SymbolOffsetExpr *>(arg)->GetSymbol();
            switch (sym->GetKind()) {
              case Global::Kind::EXTERN:
              case Global::Kind::BLOCK:
              case Global::Kind::FUNC: {
                // Pointers into functions are UB.
                return;
              }
              case Global::Kind::ATOM: {
                auto *obj = static_cast<Atom *>(sym)->getParent();
                taint.Add(objs_->objectMap_.Map(obj));
                return;
              }
            }
            llvm_unreachable("not implemented");
          }
        }
        llvm_unreachable("invalid expression kind");
      }
    }
    llvm_unreachable("invalid value kind");
  }

  void Visit(Inst *inst) override {}

private:
  template<typename T>
  TaintedObjects::BlockInfo *VisitCall(
      TaintedObjects::BlockInfo &info,
      const CallSite<T> &call)
  {
    auto cs = cs_.Append(&call);
    if (auto *mov = ::dyn_cast_or_null<const MovInst>(call.GetCallee())) {
      auto *callee = mov->GetArg();
      switch (callee->GetKind()) {
        case Value::Kind::INST: {
          auto ret = objs_->CreateBlock();
          objs_->indirectCalls_.emplace_back(cs, id_, ret);
          return objs_->blocks_.Find(ret);
        }
        case Value::Kind::GLOBAL: {
          switch (static_cast<Global *>(callee)->GetKind()) {
            case Global::Kind::EXTERN: {
              auto *ext = static_cast<Extern *>(callee);
              std::string name(ext->GetName());
              auto it = kCallbacks.find(name);
              if (it == kCallbacks.end() || it->second) {
                llvm_unreachable("not implemented");
              } else {
                return objs_->blocks_.Find(id_);
              }
            }
            case Global::Kind::FUNC: {
              Func &func = *static_cast<Func *>(callee);
              info.Successors.Insert(objs_->Explore(cs, func));
              return objs_->blocks_.Find(objs_->MapFunc(cs, &func));
            }
            case Global::Kind::BLOCK:
            case Global::Kind::ATOM: {
              // Undefined behaviour - no taint.
              return nullptr;
            }
          }
          llvm_unreachable("invalid global kind");
        }
        case Value::Kind::CONST:
        case Value::Kind::EXPR: {
          // Undefined behaviour - no taint.
          return nullptr;
        }
      }
      llvm_unreachable("invalid value kind");
    } else {
      auto ret = objs_->CreateBlock();
      objs_->indirectCalls_.emplace_back(cs, id_, ret);
      return objs_->blocks_.Find(ret);
    }
  }

private:
  TaintedObjects *objs_;
  ID<TaintedObjects::BlockInfo> id_;
  TaintedObjects::BlockInfo *&info_;
  const TaintedObjects::CallString<TaintedObjects::N> &cs_;
};

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::Explore(
    const CallString<N> &cs,
    Func &func)
{
  const Inst *entry = &*func.getEntryBlock().begin();
  std::pair<CallString<N>, const Inst *> key{ cs, entry };
  if (auto it = instToBlock_.find(key); it != instToBlock_.end()) {
    return it->second;
  }

  for (auto &block : func) {
    ID<BlockInfo> blockID = MapInst(cs, &*block.begin());
    BlockInfo *blockInfo = blocks_.Find(blockID);
    for (auto &inst : block) {
      BlockBuilder(this, blockID, blockInfo, cs).Dispatch(&inst);
    }
  }

  return instToBlock_.find(key)->second;
}

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::Explore(
    const CallString<N> &cs,
    Block &block)
{
  Inst *entry = &*block.begin();
  std::pair<CallString<N>, const Inst *> key{ cs, entry };
  if (auto it = instToBlock_.find(key); it != instToBlock_.end()) {
    return it->second;
  }

  ID<BlockInfo> blockID = MapInst(cs, entry);
  BlockInfo *blockInfo = blocks_.Find(blockID);
  for (auto &inst : block) {
    BlockBuilder(this, blockID, blockInfo, cs).Dispatch(&inst);
  }
  return blockID;
}

// -----------------------------------------------------------------------------
void TaintedObjects::Propagate()
{
  while (!queue_.Empty()) {
    auto nodeID = queue_.Pop();
    BlockInfo *node = blocks_.Find(nodeID);

    for (auto succID : node->Successors) {
      BlockInfo *succ = blocks_.Find(succID);
      if (succ->Taint.Union(node->Taint)) {
        queue_.Push(succID);
      }
    }
  }
}

// -----------------------------------------------------------------------------
bool TaintedObjects::ExpandIndirect()
{
  bool changed = false;
  auto indirectJumps = indirectJumps_;
  for (auto &jump : indirectJumps) {
    auto *node = blocks_.Find(jump.From);
    bool expanded = false;
    for (auto blockID : node->Taint.blocks()) {
      auto *block = blockMap_.Map(blockID);
      Explore(jump.CS, *block);
      if (node->Successors.Insert(MapBlock(jump.CS, block))) {
        expanded = true;
      }
    }
    if (expanded) {
      changed = true;
      queue_.Push(jump.From);
    }
  }

  auto indirectCalls = indirectCalls_;
  for (auto &call : indirectCalls) {
    auto *node = blocks_.Find(call.From);
    bool expanded = false;
    for (auto funcID : node->Taint.funcs()) {
      auto *func = funcMap_.Map(funcID);
      Explore(call.CS, *func);

      auto entryID = MapBlock(call.CS, &func->getEntryBlock());
      auto retID = MapFunc(call.CS, func);
      auto *ret = blocks_.Find(retID);
      if (node->Successors.Insert(entryID) || ret->Successors.Insert(call.Cont)) {
        expanded = true;
      }
    }
    if (expanded) {
      changed = true;
      queue_.Push(call.From);
    }
  }

  return changed;
}

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::MapInst(
    const CallString<N> &cs,
    Inst *inst)
{
  std::pair<CallString<N>, const Inst *> key{ cs, inst };
  if (auto it = instToBlock_.find(key); it != instToBlock_.end()) {
    return it->second;
  }

  auto id = blocks_.Emplace();
  instToBlock_.insert({key, id});
  queue_.Push(id);
  blockSites_[inst].Insert(id);
  return id;
}

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::MapFunc(
    const CallString<N> &cs,
    Func *func)
{
  std::pair<CallString<N>, const Func *> key{ cs, func };
  if (auto it = exitToBlock_.find(key); it != exitToBlock_.end()) {
    return it->second;
  }

  auto id = blocks_.Emplace();
  exitToBlock_.insert({key, id});
  queue_.Push(id);
  return id;
}

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::MapBlock(
    const CallString<N> &cs,
    Block *block)
{
  return MapInst(cs, &*block->begin());
}

// -----------------------------------------------------------------------------
ID<TaintedObjects::BlockInfo> TaintedObjects::CreateBlock()
{
  auto id = blocks_.Emplace();
  queue_.Push(id);
  return id;
}
