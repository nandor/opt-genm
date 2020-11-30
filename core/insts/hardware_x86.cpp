// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include "core/cast.h"
#include "core/inst.h"
#include "core/insts/hardware_x86.h"



// -----------------------------------------------------------------------------
X86_XchgInst::X86_XchgInst(
    Type type,
    Ref<Inst> addr,
    Ref<Inst> val,
    AnnotSet &&annot)
  : MemoryInst(Kind::X86_XCHG, 2, std::move(annot))
  , type_(type)
{
  Set<0>(addr);
  Set<1>(val);
}

// -----------------------------------------------------------------------------
unsigned X86_XchgInst::GetNumRets() const
{
  return 1;
}

// -----------------------------------------------------------------------------
Type X86_XchgInst::GetType(unsigned i) const
{
  if (i == 0) return type_;
  llvm_unreachable("invalid operand");
}


// -----------------------------------------------------------------------------
ConstRef<Inst> X86_XchgInst::GetAddr() const
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_XchgInst::GetAddr()
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
ConstRef<Inst> X86_XchgInst::GetVal() const
{
  return cast<Inst>(Get<1>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_XchgInst::GetVal()
{
  return cast<Inst>(Get<1>());
}

// -----------------------------------------------------------------------------
X86_CmpXchgInst::X86_CmpXchgInst(
    Type type,
    Ref<Inst> addr,
    Ref<Inst> val,
    Ref<Inst> ref,
    AnnotSet &&annot)
  : MemoryInst(Kind::X86_CMPXCHG, 3, std::move(annot))
  , type_(type)
{
  Set<0>(addr);
  Set<1>(val);
  Set<2>(ref);
}

// -----------------------------------------------------------------------------
unsigned X86_CmpXchgInst::GetNumRets() const
{
  return 1;
}

// -----------------------------------------------------------------------------
Type X86_CmpXchgInst::GetType(unsigned i) const
{
  if (i == 0) return type_;
  llvm_unreachable("invalid operand");
}


// -----------------------------------------------------------------------------
ConstRef<Inst> X86_CmpXchgInst::GetAddr() const
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_CmpXchgInst::GetAddr()
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
ConstRef<Inst> X86_CmpXchgInst::GetVal() const
{
  return cast<Inst>(Get<1>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_CmpXchgInst::GetVal()
{
  return cast<Inst>(Get<1>());
}

// -----------------------------------------------------------------------------
ConstRef<Inst> X86_CmpXchgInst::GetRef() const
{
  return cast<Inst>(Get<2>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_CmpXchgInst::GetRef()
{
  return cast<Inst>(Get<2>());
}

// -----------------------------------------------------------------------------
X86_FPUControlInst::X86_FPUControlInst(Kind kind, Ref<Inst> addr, AnnotSet &&annot)
  : Inst(kind, 1, std::move(annot))
{
  Set<0>(addr);
}

// -----------------------------------------------------------------------------
unsigned X86_FPUControlInst::GetNumRets() const
{
  return 0;
}

// -----------------------------------------------------------------------------
Type X86_FPUControlInst::GetType(unsigned i) const
{
  llvm_unreachable("invalid operand");
}

// -----------------------------------------------------------------------------
ConstRef<Inst> X86_FPUControlInst::GetAddr() const
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
Ref<Inst> X86_FPUControlInst::GetAddr()
{
  return cast<Inst>(Get<0>());
}

// -----------------------------------------------------------------------------
X86_FnStCwInst::X86_FnStCwInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_FNSTCW, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_FnStSwInst::X86_FnStSwInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_FNSTSW, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_FnStEnvInst::X86_FnStEnvInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_FNSTENV, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_FLdCwInst::X86_FLdCwInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_FLDCW, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_FLdEnvInst::X86_FLdEnvInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_FLDENV, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_LdmXCSRInst::X86_LdmXCSRInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_LDMXCSR, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_StmXCSRInst::X86_StmXCSRInst(Ref<Inst> addr, AnnotSet &&annot)
  : X86_FPUControlInst(Kind::X86_STMXCSR, addr, std::move(annot))
{
}

// -----------------------------------------------------------------------------
X86_FnClExInst::X86_FnClExInst(AnnotSet &&annot)
  : Inst(Kind::X86_FNCLEX, 0, std::move(annot))
{
}

// -----------------------------------------------------------------------------
unsigned X86_FnClExInst::GetNumRets() const
{
  return 0;
}

// -----------------------------------------------------------------------------
Type X86_FnClExInst::GetType(unsigned i) const
{
  llvm_unreachable("invalid index");
}


// -----------------------------------------------------------------------------
X86_RdtscInst::X86_RdtscInst(Type type, AnnotSet &&annot)
  : OperatorInst(Kind::X86_RDTSC, type, 0, std::move(annot))
{
}