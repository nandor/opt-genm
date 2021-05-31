// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <stack>

#include "core/atom.h"
#include "core/object.h"
#include "core/data.h"
#include "passes/tags/refinement.h"
#include "passes/tags/tagged_type.h"
#include "passes/tags/type_analysis.h"

using namespace tags;



// -----------------------------------------------------------------------------
Refinement::Refinement(TypeAnalysis &analysis, const Target *target, Func &func)
  : analysis_(analysis)
  , target_(target)
  , func_(func)
  , dt_(func_)
  , pdt_(func_)
{
  df_.analyze(dt_);
  pdf_.analyze(pdt_);
}

// -----------------------------------------------------------------------------
void Refinement::Run()
{
  for (Block &block : func_) {
    for (Inst &inst : block) {
      Dispatch(inst);
    }
  }

  while (!queue_.empty()) {
    auto *inst = queue_.front();
    queue_.pop();
    inQueue_.erase(inst);
    Dispatch(*inst);
  }
}

// -----------------------------------------------------------------------------
void Refinement::Refine(
    Block *parent,
    Ref<Inst> ref,
    const TaggedType &type)
{
  if (pdt_.dominates(parent, ref->getParent())) {
    // If the definition is post-dominated by the use, change its type.
    analysis_.Refine(ref, type);
    auto *source = &*ref;
    if (inQueue_.insert(source).second) {
      queue_.push(source);
    }
  } else {
    // Find the post-dominated nodes which are successors of the frontier.
    std::unordered_map<const Block *, TaggedType> splits;
    for (auto *front : pdf_.calculate(pdt_, pdt_.getNode(parent))) {
      for (auto *succ : front->successors()) {
        if (pdt_.dominates(parent, succ)) {
          splits.emplace(succ, type);
        }
      }
    }

    // Find the set of nodes which lead into a use of the references.
    DefineSplits(ref, splits);
  }
}

// -----------------------------------------------------------------------------
void Refinement::Refine(
    Block *start,
    Block *end,
    Ref<Inst> ref,
    const TaggedType &type)
{
  if (pdt_.Dominates(start, end, ref->getParent())) {
    // If the definition is post-dominated by the edge, change its type.
    analysis_.Refine(ref, type);
    auto *source = &*ref;
    if (inQueue_.insert(source).second) {
      queue_.push(source);
    }
  } else {
    // Find the post-dominated nodes which are successors of the frontier.
    std::unordered_map<const Block *, TaggedType> splits;
    for (auto *front : pdf_.calculate(pdt_, pdt_.getNode(start))) {
      for (auto *succ : front->successors()) {
        if (pdt_.Dominates(start, end, succ)) {
          splits.emplace(succ, type);
        }
      }
    }

    // Find the set of nodes which lead into a use of the references.
    DefineSplits(ref, splits);
  }
}

// -----------------------------------------------------------------------------
void Refinement::RefineAddr(Inst &inst, Ref<Inst> addr)
{
  switch (analysis_.Find(addr).GetKind()) {
    case TaggedType::Kind::UNKNOWN:
    case TaggedType::Kind::ZERO:
    case TaggedType::Kind::EVEN:
    case TaggedType::Kind::ONE:
    case TaggedType::Kind::ODD:
    case TaggedType::Kind::ZERO_ONE:
    case TaggedType::Kind::INT:
    case TaggedType::Kind::UNDEF: {
      // This is a trap, handled elsewhere.
      return;
    }
    case TaggedType::Kind::YOUNG:
    case TaggedType::Kind::HEAP:
    case TaggedType::Kind::PTR: {
      // Already a pointer, nothing to refine.
      return;
    }
    case TaggedType::Kind::VAL: {
      // Refine to HEAP.
      Refine(inst.getParent(), addr, TaggedType::Heap());
      return;
    }
    case TaggedType::Kind::PTR_NULL:
    case TaggedType::Kind::PTR_INT:
    case TaggedType::Kind::ANY: {
      // Refine to PTR.
      Refine(inst.getParent(), addr, TaggedType::Ptr());
      return;
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::RefineInt(Inst &inst, Ref<Inst> addr)
{
  switch (analysis_.Find(addr).GetKind()) {
    case TaggedType::Kind::UNKNOWN:
    case TaggedType::Kind::ZERO:
    case TaggedType::Kind::EVEN:
    case TaggedType::Kind::ONE:
    case TaggedType::Kind::ODD:
    case TaggedType::Kind::ZERO_ONE:
    case TaggedType::Kind::INT:
    case TaggedType::Kind::UNDEF: {
      // This is a trap, handled elsewhere.
      return;
    }
    case TaggedType::Kind::YOUNG:
    case TaggedType::Kind::HEAP:
    case TaggedType::Kind::PTR: {
      // Already a pointer, nothing to refine.
      return;
    }
    case TaggedType::Kind::VAL: {
      // Refine to ODD.
      Refine(inst.getParent(), addr, TaggedType::Odd());
      return;
    }
    case TaggedType::Kind::PTR_NULL: {
      // Refine to ZERO.
      Refine(inst.getParent(), addr, TaggedType::Zero());
      return;

    }
    case TaggedType::Kind::PTR_INT:
    case TaggedType::Kind::ANY: {
      // Refine to INT.
      Refine(inst.getParent(), addr, TaggedType::Int());
      return;
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::RefineEquality(
    Ref<Inst> lhs,
    Ref<Inst> rhs,
    Block *b,
    Block *bt,
    Block *bf)
{
  auto vl = analysis_.Find(lhs);
  auto vr = analysis_.Find(rhs);
  if (!vl.IsUnknown() && vl < vr) {
    Specialise(rhs, b, { { vl, bt } });
    return;
  }
  if (!vr.IsUnknown() && vr < vl) {
    Specialise(lhs, b, { { vr, bt } });
    return;
  }
}

// -----------------------------------------------------------------------------
void Refinement::RefineAndOne(Ref<Inst> arg, Block *b, Block *bt, Block *bf)
{
  switch (analysis_.Find(arg).GetKind()) {
    case TaggedType::Kind::UNKNOWN:
    case TaggedType::Kind::UNDEF:
    case TaggedType::Kind::ANY: {
      return;
    }
    case TaggedType::Kind::ZERO:
    case TaggedType::Kind::ONE:
    case TaggedType::Kind::EVEN:
    case TaggedType::Kind::ODD: {
      // Can simplify condition here, always 0 or 1.
      return;
    }
    case TaggedType::Kind::YOUNG:
    case TaggedType::Kind::HEAP: {
      // Can simplify condition here, always 0.
      return;
    }
    case TaggedType::Kind::ZERO_ONE: {
      Specialise(arg, b, { { TaggedType::One(), bt }, { TaggedType::Zero(), bf } });
      return;
    }
    case TaggedType::Kind::INT: {
      Specialise(arg, b, { { TaggedType::Odd(), bt }, { TaggedType::Even(), bf } });
      return;
    }
    case TaggedType::Kind::VAL: {
      Specialise(arg, b, { { TaggedType::Odd(), bt }, { TaggedType::Heap(), bf } });
      return;
    }
    case TaggedType::Kind::PTR:
    case TaggedType::Kind::PTR_NULL: {
      Specialise(arg, b, { { TaggedType::Ptr(), bt }, { TaggedType::Heap(), bf } });
      return;
    }
    case TaggedType::Kind::PTR_INT: {
      return;
    }
  }
  llvm_unreachable("invalid type kind");
}

// -----------------------------------------------------------------------------
void Refinement::Specialise(
    Ref<Inst> ref,
    const Block *from,
    const std::vector<std::pair<TaggedType, Block *>> &branches)
{
  std::unordered_map<const Block *, TaggedType> splits;
  for (auto &[ty, block] : branches) {
    if (dt_.Dominates(from, block, block)) {
      splits.emplace(block, ty);
    }
  }
  DefineSplits(ref, splits);
}

// -----------------------------------------------------------------------------
std::set<Block *> Refinement::Liveness(
    Ref<Inst> ref,
    const llvm::SmallPtrSetImpl<const Block *> &defs)
{
  std::queue<Block *> q;
  for (Use &use : ref->uses()) {
    if ((*use).Index() != ref.Index()) {
      continue;
    }
    if (auto *phi = ::cast_or_null<PhiInst>(use.getUser())) {
      for (unsigned i = 0, n = phi->GetNumIncoming(); i < n; ++i) {
        if (phi->GetValue(i) == ref) {
          q.push(phi->GetBlock(i));
        }
      }
    } else {
      q.push(::cast<Inst>(use.getUser())->getParent());
    }
  }
  std::set<Block *> live;
  while (!q.empty()) {
    Block *b = q.front();
    q.pop();
    if (defs.count(b) || b == ref->getParent()) {
      continue;
    }
    if (live.insert(b).second) {
      for (Block *pred : b->predecessors()) {
        q.push(pred);
      }
    }
  }
  return live;
}

// -----------------------------------------------------------------------------
void Refinement::DefineSplits(
    Ref<Inst> ref,
    const std::unordered_map<const Block *, TaggedType> &splits)
{
  llvm::SmallPtrSet<const Block *, 8> blocks;
  for (auto &[block, ty] : splits) {
    blocks.insert(block);
  }

  auto refTy = analysis_.Find(ref);
  auto live = Liveness(ref, blocks);

  // Place the PHIs for the blocks.
  std::unordered_map<Block *, PhiInst *> phis;
  std::unordered_map<PhiInst *, TaggedType> newPhis;
  {
    std::queue<const Block *> q;
    for (const Block *block : blocks) {
      q.push(block);
    }
    while (!q.empty()) {
      const Block *block = q.front();
      q.pop();
      for (auto front : df_.calculate(dt_, dt_.getNode(block))) {
        if (live.count(front) && !phis.count(front)) {
          auto *phi = new PhiInst(ref.GetType(), {});
          front->AddPhi(phi);
          TaggedType ty = TaggedType::Unknown();
          for (Block *pred : front->predecessors()) {
            phi->Add(pred, ref);
            TaggedType predTy = TaggedType::Unknown();
            for (auto &[block, ty] : splits) {
              if (dt_.dominates(block, pred)) {
                predTy |= ty;
              }
            }
            ty |= predTy.IsUnknown() ? refTy : predTy;
          }
          phis.emplace(front, phi);
          newPhis.emplace(phi, ty);
          q.push(front);
        }
      }
    }
  }

  std::stack<Inst *> defs;
  std::unordered_map<MovInst *, TaggedType> newMovs;
  std::function<void(Block *)> rewrite = [&](Block *block)
  {
    Block::iterator begin;
    bool defined = false;
    if (auto it = splits.find(block); it != splits.end()) {
      bool liveOut = false;
      for (Block *b : block->successors()) {
        if (live.count(b)) {
          liveOut = true;
          break;
        }
      }
      // Register the value, if defined in block.
      if (liveOut) {
        auto arg = defs.empty() ? ref : defs.top()->GetSubValue(0);
        auto *mov = new MovInst(ref.GetType(), arg, {});
        block->insert(mov, block->first_non_phi());
        defs.push(mov);
        newMovs.emplace(mov, it->second);
        begin = std::next(mov->getIterator());
        defined = true;
      } else {
        begin = block->first_non_phi();
      }
    } else {
      if (auto it = phis.find(block); it != phis.end()) {
        // Register value, if defined in PHI.
        defs.push(it->second);
        defined = true;
      }
      begin = block->first_non_phi();
    }
    // Rewrite, if there are uses to be rewritten.
    if (!defs.empty()) {
      auto mov = defs.top()->GetSubValue(0);
      for (auto it = begin; it != block->end(); ++it) {
        for (Use &use : it->operands()) {
          if (::cast_or_null<Inst>(*use) == ref) {
            use = mov;
          }
        }
      }
      for (Block *succ : block->successors()) {
        for (PhiInst &phi : succ->phis()) {
          if (phi.GetValue(block) == ref) {
            phi.Remove(block);
            phi.Add(block, mov);
          }
        }
      }
    }
    // Rewrite dominated nodes.
    for (auto *child : *dt_[block]) {
      rewrite(child->getBlock());
    }
    // Remove the definition.
    if (defined) {
      defs.pop();
    }
  };
  rewrite(dt_.getRoot());

  // Recompute the types of the users of the refined instructions.
  for (auto &[mov, type] : newMovs) {
    assert(mov->use_size() > 0 && "dead mov");
    analysis_.Define(mov->GetSubValue(0), type);
  }
  // Schedule the PHIs to be recomputed.
  for (auto &[phi, type] : newPhis) {
    assert(phi->use_size() > 0 && "dead phi");
    analysis_.Define(phi->GetSubValue(0), type);
  }
  // Trigger an update of anything relying on the reference.
  analysis_.Refine(ref, analysis_.Find(ref));
}

// -----------------------------------------------------------------------------
void Refinement::VisitMemoryLoadInst(MemoryLoadInst &i)
{
  RefineAddr(i, i.GetAddr());
}

// -----------------------------------------------------------------------------
void Refinement::VisitMemoryStoreInst(MemoryStoreInst &i)
{
  RefineAddr(i, i.GetAddr());
}

// -----------------------------------------------------------------------------
void Refinement::VisitSelectInst(SelectInst &i)
{
  auto vo = analysis_.Find(i);
  auto vt = analysis_.Find(i.GetTrue());
  auto vf = analysis_.Find(i.GetFalse());
  if (!(vt <= vo)) {
    Refine(i.getParent(), i.GetTrue(), vo);
  }
  if (!(vf <= vo)) {
    Refine(i.getParent(), i.GetFalse(), vo);
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitSubInst(SubInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto vo = analysis_.Find(i);
  if (vo.IsPtr() && vl.IsPtrUnion() && vr.IsIntLike()) {
    RefineAddr(i, i.GetLHS());
    return;
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitAddInst(AddInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto vo = analysis_.Find(i);
  if (vo.IsPtr()) {
    if (vl.IsIntLike() && vr.IsPtrUnion()) {
      RefineAddr(i, i.GetRHS());
    }
    if (vr.IsIntLike() && vl.IsPtrUnion()) {
      RefineAddr(i, i.GetLHS());
    }
    if (vl.IsPtrLike() && vr.IsPtrUnion()) {
      RefineInt(i, i.GetRHS());
    }
    if (vr.IsPtrLike() && vl.IsPtrUnion()) {
      RefineInt(i, i.GetLHS());
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitCmpInst(CmpInst &i)
{
  auto cc = i.GetCC();
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());

  if (!IsEquality(cc) || IsOrdered(cc)) {
    if (vl.IsVal() && vr.IsOdd()) {
      return Refine(i.getParent(), i.GetLHS(), TaggedType::Odd());
    }
    if (vr.IsVal() && vl.IsOdd()) {
      return Refine(i.getParent(), i.GetRHS(), TaggedType::Odd());
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitAndInst(AndInst &i)
{
}

// -----------------------------------------------------------------------------
void Refinement::VisitOrInst(OrInst &i)
{
}

// -----------------------------------------------------------------------------
void Refinement::VisitXorInst(XorInst &i)
{
}

// -----------------------------------------------------------------------------
void Refinement::VisitMovInst(MovInst &i)
{
  if (auto arg = ::cast_or_null<Inst>(i.GetArg())) {
    auto va = analysis_.Find(arg);
    auto vo = analysis_.Find(i);
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitPhiInst(PhiInst &phi)
{
  auto vphi = analysis_.Find(phi);
  if (vphi.IsUnknown()) {
    return;
  }

  auto *parent = phi.getParent();
  for (unsigned i = 0, n = phi.GetNumIncoming(); i < n; ++i) {
    auto ref = phi.GetValue(i);
    auto block = phi.GetBlock(i);
    if (vphi < analysis_.Find(ref)) {
      Refine(phi.GetBlock(i), parent, ref, vphi);
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitArgInst(ArgInst &arg)
{
  auto ty = analysis_.Find(arg);
  if (ty.IsUnknown()) {
    return;
  }
  Func *f = arg.getParent()->getParent();
  for (auto *user : f->users()) {
    auto mov = ::cast_or_null<MovInst>(user);
    if (!mov) {
      continue;
    }
    for (auto *movUser : mov->users()) {
      auto call = ::cast_or_null<CallSite>(movUser);
      if (!call || call->GetCallee() != mov->GetSubValue(0)) {
        continue;
      }
      if (call->arg_size() > arg.GetIndex()) {
        auto argRef = call->arg(arg.GetIndex());
        auto argTy = analysis_.Find(argRef);
        if (ty < argTy) {
          Refine(call->getParent(), argRef, ty);
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitCallSite(CallSite &site)
{
  auto callee = analysis_.Find(site.GetCallee());
  if (callee.IsUnknown()) {
    return;
  }

  // Refine the callee to a pointer.
  RefineAddr(site, site.GetCallee());

  // Refine return values of calles.
  std::vector<TaggedType> rets;
  for (unsigned i = 0, n = site.GetNumRets(); i < n; ++i) {
    auto val = analysis_.Find(site.GetSubValue(i));
    if (val.IsUnknown()) {
      return;
    }
    rets.push_back(val);
  }

  auto less = [&rets, this] (Func *f)
  {
    auto it = analysis_.rets_.find(f);
    assert(it != analysis_.rets_.end() && "missing returns");
    auto &otherRets = it->second;
    for (unsigned i = 0, n = rets.size(); i < n; ++i) {
      if (i < otherRets.size()) {
        if (!(rets[i] < otherRets[i])) {
          return false;
        }
      } else {
        llvm_unreachable("not implemented");
      }
    }
    return true;
  };

  if (auto *f = site.GetDirectCallee(); f && !rets.empty()) {
    std::queue<Func *> q;
    q.push(f);

    while (!q.empty()) {
      auto *f = q.front();
      q.pop();

      if (less(f)) {
        // Update cached return values.
        auto &otherRets = analysis_.rets_[f];
        for (unsigned i = 0, n = otherRets.size(); i < n; ++i) {
          if (i < rets.size()) {
            otherRets[i] = rets[i];
          } else {
            llvm_unreachable("not implemented");
          }
        }

        // Propagate in function.
        for (Block &block : *f) {
          auto *term = block.GetTerminator();
          if (!term->IsReturn()) {
            continue;
          }
          if (auto *ret = ::cast_or_null<ReturnInst>(term)) {
            for (unsigned i = 0, n = ret->arg_size(); i < n; ++i) {
              auto argRef = ret->arg(i);
              if (i < rets.size()) {
                auto val = analysis_.Find(argRef);
                if (rets[i] < val) {
                  Refine(&block, argRef, rets[i]);
                }
              } else {
                llvm_unreachable("not implemented");
              }
            }
          }
          if (auto *tcall = ::cast_or_null<TailCallInst>(term)) {
            if (auto *callee = tcall->GetDirectCallee()) {
              q.push(callee);
            }
          }
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
void Refinement::VisitJumpCondInst(JumpCondInst &jcc)
{
  auto *bt = jcc.GetTrueTarget();
  auto *bf = jcc.GetFalseTarget();
  if (auto inst = ::cast_or_null<CmpInst>(jcc.GetCond())) {
    auto l = inst->GetLHS();
    auto r = inst->GetRHS();
    switch (inst->GetCC()) {
      case Cond::EQ: case Cond::UEQ: case Cond::OEQ: {
        return RefineEquality(l, r, inst->getParent(), bt, bf);
      }
      case Cond::NE: case Cond::UNE: case Cond::ONE: {
        return RefineEquality(l, r, inst->getParent(), bf, bt);
      }
      default: {
        return;
      }
    }
    llvm_unreachable("invalid condition code");
  }
  if (auto inst = ::cast_or_null<AndInst>(jcc.GetCond())) {
    if (analysis_.Find(jcc.GetCond()).IsZeroOne()) {
      auto l = inst->GetLHS();
      auto r = inst->GetRHS();
      if (analysis_.Find(l).IsOne()) {
        return RefineAndOne(r, jcc.getParent(), bt, bf);
      }
      if (analysis_.Find(r).IsOne()) {
        return RefineAndOne(l, jcc.getParent(), bt, bf);
      }
    }
  }
}