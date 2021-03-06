// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include "core/cast.h"
#include "passes/tags/step.h"
#include "passes/tags/register_analysis.h"

using namespace tags;



// -----------------------------------------------------------------------------
TaggedType Step::Clamp(TaggedType type, Type ty)
{
  if (ty == Type::V64) {
    switch (type.GetKind()) {
      case TaggedType::Kind::UNKNOWN:   return TaggedType::Unknown();
      case TaggedType::Kind::PTR_INT:   return TaggedType::Val();
      case TaggedType::Kind::VAL:       return TaggedType::Val();
      case TaggedType::Kind::PTR:       return TaggedType::Heap();
      case TaggedType::Kind::HEAP:      return TaggedType::Heap();
      case TaggedType::Kind::HEAP_OFF:  return TaggedType::Heap();
      case TaggedType::Kind::YOUNG:     return TaggedType::Young();
      case TaggedType::Kind::UNDEF:     return TaggedType::Undef();
      case TaggedType::Kind::PTR_NULL:  return TaggedType::Heap();
      case TaggedType::Kind::ADDR_NULL: return TaggedType::Undef();
      case TaggedType::Kind::ADDR:      return TaggedType::Undef();
      case TaggedType::Kind::FUNC:      return TaggedType::Func();
      case TaggedType::Kind::ADDR_INT: {
        return kind_ == Kind::REFINE ? TaggedType::Odd() : TaggedType::Val();
      }
      case TaggedType::Kind::INT: {
        const auto &m = type.GetInt();
        return TaggedType::Mask(MaskedType(m.GetValue() | 1, m.GetKnown() | 1));
      }
    }
    llvm_unreachable("invalid value kind");
  } else if (ty == Type::I64) {
    return type.IsHeapOff() ? TaggedType::Addr() : type;
  } else {
    return type;
  }
}

// -----------------------------------------------------------------------------
void Step::VisitCallSite(CallSite &call)
{
  Func *caller = call.getParent()->getParent();
  if (auto *f = call.GetDirectCallee()) {
    // Only evaluate if all args are known.
    llvm::SmallVector<TaggedType, 8> args;
    for (unsigned i = 0, n = call.arg_size(); i < n; ++i) {
      auto arg = analysis_.Find(call.arg(i));
      if (arg.IsUnknown()) {
        return;
      }
      if (IsCamlCall(f->GetCallingConv())) {
        switch (i) {
          case 0: {
            args.push_back(TaggedType::Ptr());
            break;
          }
          case 1: {
            args.push_back(TaggedType::Young());
            break;
          }
          default: {
            args.push_back(arg);
            break;
          }
        }
      } else {
        args.push_back(arg);
      }
    }
    if (kind_ == Kind::REFINE && !f->IsRoot() && !f->HasAddressTaken()) {
      for (auto *user : f->users()) {
        auto mov = ::cast_or_null<MovInst>(user);
        if (!mov) {
          continue;
        }
        auto movRef = mov->GetSubValue(0);
        for (auto *movUser : mov->users()) {
          auto otherCall = ::cast_or_null<CallSite>(movUser);
          if (!otherCall || otherCall == &call || otherCall->GetCallee() != movRef) {
            continue;
          }
          auto otherParent = otherCall->getParent()->getParent();
          for (unsigned i = 0, n = otherCall->arg_size(); i < n; ++i) {
            if (args.size() <= i) {
              args.resize(i + 1, TaggedType::Undef());
            }

            auto ref = otherCall->arg(i);
            auto arg = ::cast_or_null<ArgInst>(ref);
            if (otherParent != f || !arg || arg->GetIndex() != i) {
              args[i] |= analysis_.Find(ref);
            }
          }
        }
      }

      for (unsigned i = 0, n = f->params().size(); i < n; ++i) {
        for (auto *arg : analysis_.args_[{ f, i }]) {
          if (i < args.size()) {
            Mark(arg->GetSubValue(0), Clamp(args[i], arg->GetType()));
          } else {
            llvm_unreachable("not implemented");
          }
        }
      }
    } else {
      // Propagate values to arguments.
      for (unsigned i = 0, n = call.arg_size(); i < n; ++i) {
        auto type = args[i];
        for (auto *inst : analysis_.args_[std::make_pair(f, i)]) {
          auto ref = inst->GetSubValue(0);
          auto arg = Clamp(analysis_.Find(ref) | type, inst->GetType());
          Mark(ref, arg);
        }
      }
    }
    // If the callee recorded a value already, propagate it.
    if (auto it = analysis_.rets_.find(f); it != analysis_.rets_.end()) {
      if (auto *tcall = ::cast_or_null<const TailCallInst>(&call)) {
        std::vector<TaggedType> values;
        for (unsigned i = 0, n = tcall->type_size(); i < n; ++i) {
          if (i < it->second.size()) {
            values.push_back(it->second[i]);
          } else {
            llvm_unreachable("not implemented");
          }
        }
        Return(caller, tcall, values);
      } else {
        for (unsigned i = 0, n = call.GetNumRets(); i < n; ++i) {
          if (i < it->second.size()) {
            Mark(call.GetSubValue(i), Clamp(it->second[i], call.type(i)));
          } else {
            llvm_unreachable("not implemented");
          }
        }
      }
    }
  } else {
    switch (call.GetCallingConv()) {
      case CallingConv::SETJMP:
      case CallingConv::XEN:
      case CallingConv::INTR:
      case CallingConv::MULTIBOOT:
      case CallingConv::WIN64:
      case CallingConv::C: {
        if (auto *tcall = ::cast_or_null<const TailCallInst>(&call)) {
          std::vector<TaggedType> values(call.type_size(), TaggedType::PtrInt());
          Return(caller, tcall, values);
        } else {
          for (unsigned i = 0, n = call.GetNumRets(); i < n; ++i) {
            Mark(call.GetSubValue(i), TaggedType::PtrInt());
          }
        }
        return;
      }
      case CallingConv::CAML: {
        if (target_) {
          if (auto *tcall = ::cast_or_null<const TailCallInst>(&call)) {
            std::vector<TaggedType> values;
            values.push_back(TaggedType::Ptr());
            values.push_back(TaggedType::Young());
            for (unsigned i = 2, n = call.type_size(); i < n; ++i) {
              values.push_back(Infer(call.type(i)));
            }
            Return(caller, tcall, values);
          } else {
            Mark(call.GetSubValue(0), TaggedType::Ptr());
            Mark(call.GetSubValue(1), TaggedType::Young());
            for (unsigned i = 2, n = call.GetNumRets(); i < n; ++i) {
              auto ref = call.GetSubValue(i);
              Mark(ref, Infer(ref.GetType()));
            }
          }
        } else {
          llvm_unreachable("not implemented");
        }
        return;
      }
      case CallingConv::CAML_ALLOC: {
        if (target_) {
          Mark(call.GetSubValue(0), TaggedType::Ptr());
          Mark(call.GetSubValue(1), TaggedType::Young());
        } else {
          llvm_unreachable("not implemented");
        }
        return;
      }
      case CallingConv::CAML_GC: {
        if (target_) {
          Mark(call.GetSubValue(0), TaggedType::Ptr());
          Mark(call.GetSubValue(1), TaggedType::Young());
        } else {
          llvm_unreachable("not implemented");
        }
        return;
      }
    }
    llvm_unreachable("unknown calling convention");
  }
}

// -----------------------------------------------------------------------------
void Step::VisitMovInst(MovInst &i)
{
  if (auto inst = ::cast_or_null<Inst>(i.GetArg())) {
    auto val = Clamp(analysis_.Find(inst), i.GetType());
    if (!val.IsUnknown()) {
      Mark(i, val);
    }
  }
}

// -----------------------------------------------------------------------------
void Step::VisitAddInst(AddInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (IsIntegerType(i.GetType())) {
    auto r = Add(vl, vr);
    if (!r.IsUnknown()) {
      Mark(i, Clamp(r, i.GetType()));
    }
  } else {
    Mark(i, TaggedType::Int());
  }
}

// -----------------------------------------------------------------------------
void Step::VisitSubInst(SubInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (IsIntegerType(i.GetType())) {
    auto r = Sub(vl, vr);
    if (!r.IsUnknown()) {
      Mark(i, Clamp(r, i.GetType()));
    }
  } else {
    Mark(i, TaggedType::Int());
  }
}

// -----------------------------------------------------------------------------
void Step::VisitMulInst(MulInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (IsIntegerType(i.GetType())) {
    auto r = Mul(vl, vr);
    if (!r.IsUnknown()) {
      Mark(i, Clamp(r, i.GetType()));
    }
  } else {
    Mark(i, TaggedType::Int());
  }
}

// -----------------------------------------------------------------------------
void Step::VisitMultiplyInst(MultiplyInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (vl.IsUnknown() || vr.IsUnknown()) {
    return;
  }
  Mark(i, TaggedType::Int());
}

// -----------------------------------------------------------------------------
void Step::VisitDivisionRemainderInst(DivisionRemainderInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (vl.IsUnknown() || vr.IsUnknown()) {
    return;
  }
  Mark(i, TaggedType::Int());
}

// -----------------------------------------------------------------------------
void Step::VisitAndInst(AndInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto r = And(i.GetType(), vl, vr);
  if (!r.IsUnknown()) {
    Mark(i, r);
  }
}

// -----------------------------------------------------------------------------
void Step::VisitXorInst(XorInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto r = Xor(vl, vr);
  if (!r.IsUnknown()) {
    Mark(i, Clamp(r, i.GetType()));
  }
}

// -----------------------------------------------------------------------------
void Step::VisitOrInst(OrInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto r = Or(vl, vr);
  if (!r.IsUnknown()) {
    Mark(i, Clamp(r, i.GetType()));
  }
}

// -----------------------------------------------------------------------------
void Step::VisitShiftRightInst(ShiftRightInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto r = Shr(i.GetType(), vl, vr);
  if (!r.IsUnknown()) {
    Mark(i, r);
  }
}

// -----------------------------------------------------------------------------
void Step::VisitSllInst(SllInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  auto r = Shl(i.GetType(), vl, vr);
  if (!r.IsUnknown()) {
    Mark(i, r);
  }
}

// -----------------------------------------------------------------------------
void Step::VisitRotlInst(RotlInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (vl.IsUnknown() || vr.IsUnknown()) {
    return;
  }
  Mark(i, TaggedType::Int());
}

// -----------------------------------------------------------------------------
void Step::VisitExtensionInst(ExtensionInst &i)
{
  if (IsFloatType(i.GetType()) || IsFloatType(i.GetArg().GetType())) {
    Mark(i, TaggedType::Int());
  } else {
    auto arg = analysis_.Find(i.GetArg());
    auto ret = Ext(i.GetType(), arg);
    if (!ret.IsUnknown()) {
      Mark(i, ret);
    }
  }
}

// -----------------------------------------------------------------------------
void Step::VisitTruncInst(TruncInst &i)
{
  if (IsFloatType(i.GetType()) || IsFloatType(i.GetArg().GetType())) {
    Mark(i, TaggedType::Int());
  } else {
    auto arg = analysis_.Find(i.GetArg());
    auto ret = Trunc(i.GetType(), arg);
    if (!ret.IsUnknown()) {
      Mark(i, ret);
    }
  }
}

// -----------------------------------------------------------------------------
void Step::VisitBitCastInst(BitCastInst &i)
{
  if (IsFloatType(i.GetType()) || IsFloatType(i.GetArg().GetType())) {
    Mark(i, TaggedType::Int());
  } else {
    auto arg = analysis_.Find(i.GetArg());
    if (arg.IsUnknown()) {
      return;
    }
    Mark(i, arg);
  }
}

// -----------------------------------------------------------------------------
void Step::VisitByteSwapInst(ByteSwapInst &i)
{
  auto arg = analysis_.Find(i.GetArg());
  if (arg.IsUnknown()) {
    return;
  }
  Mark(i, TaggedType::Int());
}

// -----------------------------------------------------------------------------
void Step::VisitMemoryExchangeInst(MemoryExchangeInst &i)
{
  Mark(i, Clamp(TaggedType::PtrInt(), i.GetType()));
}

// -----------------------------------------------------------------------------
void Step::VisitMemoryCompareExchangeInst(MemoryCompareExchangeInst &i)
{
  auto addr = analysis_.Find(i.GetAddr());
  auto val = analysis_.Find(i.GetValue());
  auto ref = analysis_.Find(i.GetRef());
  if (ref.IsUnknown() || val.IsUnknown() || val.IsUnknown()) {
    return;
  }
  Mark(i, Clamp(TaggedType::PtrInt(), i.GetType()));
}

// -----------------------------------------------------------------------------
void Step::VisitCmpInst(CmpInst &i)
{
  auto vl = analysis_.Find(i.GetLHS());
  auto vr = analysis_.Find(i.GetRHS());
  if (vl.IsUnknown() || vr.IsUnknown()) {
    return;
  }

  switch (i.GetCC()) {
    case Cond::EQ: case Cond::UEQ: {
      if ((vl.IsOdd() && vr.IsEven()) || (vl.IsEven() && vr.IsOdd())) {
        Mark(i, TaggedType::Zero());
      } else {
        Mark(i, TaggedType::ZeroOne());
      }
      return;
    }
    case Cond::NE: case Cond::UNE: {
      if ((vl.IsOdd() && vr.IsEven()) || (vl.IsEven() && vr.IsOdd())) {
        Mark(i, TaggedType::One());
      } else {
        Mark(i, TaggedType::ZeroOne());
      }
      return;
    }
    case Cond::LT:
    case Cond::ULT:
    case Cond::GT:
    case Cond::UGT:
    case Cond::LE:
    case Cond::ULE:
    case Cond::GE:
    case Cond::UGE: {
      Mark(i, TaggedType::ZeroOne());
      return;
    }
    case Cond::OEQ:
    case Cond::ONE:
    case Cond::OLT:
    case Cond::OGT:
    case Cond::OLE:
    case Cond::OGE:
    case Cond::UO:
    case Cond::O: {
      Mark(i, TaggedType::ZeroOne());
      return;
    }
  }
}

// -----------------------------------------------------------------------------
void Step::VisitSelectInst(SelectInst &select)
{
  auto vt = analysis_.Find(select.GetTrue());
  auto vf = analysis_.Find(select.GetFalse());
  if (vt.IsUnknown() || vf.IsUnknown()) {
    return;
  }
  Mark(select, Clamp(vt | vf, select.GetType()));
}

// -----------------------------------------------------------------------------
void Step::VisitPhiInst(PhiInst &phi)
{
  TaggedType ty = TaggedType::Unknown();

  std::queue<PhiInst *> q;
  llvm::SmallPtrSet<PhiInst *, 8> visited;
  q.push(&phi);
  while (!q.empty()) {
    PhiInst *inst = q.front();
    q.pop();
    if (!visited.insert(inst).second) {
      continue;
    }
    for (unsigned i = 0, n = inst->GetNumIncoming(); i < n; ++i) {
      auto in = inst->GetValue(i);
      if (auto phiIn = ::cast_or_null<PhiInst>(in)) {
        q.push(&*phiIn);
      } else {
        ty |= analysis_.Find(in);
      }
    }
  }
  if (!ty.IsUnknown()) {
    Mark(phi, Clamp(ty, phi.GetType()));
  }
}

// -----------------------------------------------------------------------------
void Step::VisitReturnInst(ReturnInst &r)
{
  auto cc = r.getParent()->getParent()->GetCallingConv();

  // Collect the values returned by this function.
  std::vector<TaggedType> values;
  for (unsigned i = 0, n = r.arg_size(); i < n; ++i) {
    auto ret = analysis_.Find(r.arg(i));
    if (ret.IsUnknown()) {
      return;
    }
    switch (cc) {
      case CallingConv::SETJMP:
      case CallingConv::XEN:
      case CallingConv::INTR:
      case CallingConv::MULTIBOOT:
      case CallingConv::WIN64:
      case CallingConv::C: {
      	values.push_back(ret);
        continue;
      }
      case CallingConv::CAML:
      case CallingConv::CAML_ALLOC: 
      case CallingConv::CAML_GC: {
        switch (i) {
          case 0: values.push_back(TaggedType::Ptr()); continue;
          case 1: values.push_back(TaggedType::Young()); continue;
          default: values.push_back(ret); continue;
        }
        llvm_unreachable("invalid index");
      }
    }
    llvm_unreachable("unknown calling convention");
  }
  return Return(r.getParent()->getParent(), &r, values);
}

// -----------------------------------------------------------------------------
bool Step::Mark(Ref<Inst> inst, const TaggedType &type)
{
  switch (kind_) {
    case Kind::REFINE: {
      return analysis_.Refine(inst, type);
    }
    case Kind::FORWARD: {
      return analysis_.Mark(inst, type);
    }
  }
  llvm_unreachable("invalid kind");
}

// -----------------------------------------------------------------------------
void Step::Return(
    Func *from,
    const Inst *inst,
    const std::vector<TaggedType> &values)
{
  // Aggregate the values with those that might be returned on other paths.
  // Propagate information to the callers of the function and chain tail calls.
  std::queue<std::tuple<Func *, const Inst *, std::vector<TaggedType>>> q;
  q.emplace(from, inst, values);
  while (!q.empty()) {
    auto [f, inst, rets] = q.front();
    q.pop();

    bool changed = false;
    switch (kind_) {
      case Kind::FORWARD: {
        auto &aggregate = analysis_.rets_.emplace(
            f,
            std::vector<TaggedType>{}
        ).first->second;

        for (unsigned i = 0, n = rets.size(); i < n; ++i) {
          if (aggregate.size() <= i) {
            aggregate.resize(i, TaggedType::Undef());
            aggregate.push_back(rets[i]);
            changed = true;
          } else {
            const auto &ret = rets[i] |= aggregate[i];
            if (aggregate[i] != ret) {
              changed = true;
              aggregate[i] = ret;
            }
          }
        }
        break;
      }
      case Kind::REFINE: {
        for (Block &block : *f) {
          auto *term = block.GetTerminator();
          if (term == inst || !term->IsReturn()) {
            continue;
          }
          if (auto *ret = ::cast_or_null<ReturnInst>(term)) {
            unsigned n = ret->arg_size();
            if (n > rets.size()) {
              rets.resize(n, TaggedType::Unknown());
            }
            for (unsigned i = 0; i < n; ++i) {
              auto ref = ret->arg(i);
              auto call = ::cast_or_null<CallSite>(ref);
              if (!call || call->GetDirectCallee() != f || ref.Index() != i) {
                rets[i] |= analysis_.Find(ref);
              }
            }
            continue;
          }
          if (auto *tcall = ::cast_or_null<TailCallInst>(term)) {
            unsigned n = tcall->type_size();
            if (n > rets.size()) {
              rets.resize(n, TaggedType::Unknown());
            }
            if (auto *f = tcall->GetDirectCallee()) {
              auto it = analysis_.rets_.find(f);
              if (it != analysis_.rets_.end()) {
                for (unsigned i = 0; i < n; ++i) {
                  if (i < it->second.size()) {
                    rets[i] |= it->second[i];
                  } else {
                    rets[i] |= TaggedType::Undef();
                  }
                }
              } else {
                // No values to merge from this path.
              }
            } else {
              for (unsigned i = 0; i < n; ++i) {
                switch (tcall->GetCallingConv()) {
                  case CallingConv::SETJMP:
                  case CallingConv::XEN:
                  case CallingConv::INTR:
                  case CallingConv::MULTIBOOT:
                  case CallingConv::WIN64:
                  case CallingConv::C: {
                    rets[i] |= Infer(tcall->type(i));
                    continue;
                  }
                  case CallingConv::CAML: {
                    switch (i) {
                      case 0: {
                        rets[i] |= TaggedType::Ptr();
                        continue;
                      }
                      case 1: {
                        rets[i] |= TaggedType::Young();
                        continue;
                      }
                      default: {
                        rets[i] |= Infer(tcall->type(i));
                        continue;
                      }
                    }
                    llvm_unreachable("invalid index");
                  }
                  case CallingConv::CAML_ALLOC: {
                    llvm_unreachable("not implemented");
                  }
                  case CallingConv::CAML_GC: {
                    llvm_unreachable("not implemented");
                  }
                }
                llvm_unreachable("invalid calling convention");
              }
            }
            continue;
          }
          llvm_unreachable("invalid return instruction");
        }

        auto it = analysis_.rets_.emplace(f, rets);
        if (it.second) {
          changed = true;
        } else {
          if (!std::equal(rets.begin(), rets.end(), it.first->second.begin())) {
            changed = true;
            it.first->second = rets;
          }
        }
        break;
      }
    }

    if (changed) {
      for (auto *user : f->users()) {
        auto *mov = ::cast_or_null<MovInst>(user);
        if (!mov) {
          continue;
        }
        for (auto *movUser : mov->users()) {
          auto *call = ::cast_or_null<CallSite>(movUser);
          if (!call || call->GetCallee() != mov->GetSubValue(0)) {
            continue;
          }

          if (auto *tcall = ::cast_or_null<TailCallInst>(call)) {
            q.emplace(tcall->getParent()->getParent(), tcall, rets);
          } else {
            for (unsigned i = 0, n = call->GetNumRets(); i < n; ++i) {
              if (i < rets.size()) {
                Mark(call->GetSubValue(i), Clamp(rets[i], call->type(i)));
              } else {
                llvm_unreachable("not implemented");
              }
            }
          }
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
void Step::VisitInst(Inst &i)
{
  std::string msg;
  llvm::raw_string_ostream os(msg);
  os << i << "\n";
  llvm::report_fatal_error(msg.c_str());
}

// -----------------------------------------------------------------------------
TaggedType Step::Infer(Type ty)
{
  switch (ty) {
    case Type::V64: {
      return TaggedType::Val();
    }
    case Type::I8:
    case Type::I16:
    case Type::I32:
    case Type::I64:
    case Type::I128: {
      if (target_->GetPointerType() == ty) {
        return TaggedType::PtrInt();
      } else {
        return TaggedType::Int();
      }
    }
    case Type::F32:
    case Type::F64:
    case Type::F80:
    case Type::F128: {
      return TaggedType::Int();
    }
  }
  llvm_unreachable("invalid type");
}
