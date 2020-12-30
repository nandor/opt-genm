// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <llvm/Support/Debug.h>

#include "core/func.h"
#include "core/object.h"
#include "core/atom.h"
#include "passes/pre_eval/symbolic_object.h"
#include "passes/pre_eval/symbolic_heap.h"

#define DEBUG_TYPE "pre-eval"



// -----------------------------------------------------------------------------
SymbolicObject::SymbolicObject(llvm::Align align)
  : align_(align)
{
}

// -----------------------------------------------------------------------------
SymbolicObject::~SymbolicObject()
{
}

// -----------------------------------------------------------------------------
bool SymbolicObject::WritePrecise(
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  return Write(offset, val, type, &SymbolicObject::Set);
}

// -----------------------------------------------------------------------------
bool SymbolicObject::WriteImprecise(
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  return Write(offset, val, type, &SymbolicObject::Merge);
}

// -----------------------------------------------------------------------------
bool SymbolicObject::Write(
    int64_t offset,
    const SymbolicValue &val,
    Type type,
    bool (SymbolicObject::*mutate)(unsigned, const SymbolicValue &))
{
  // This only works for single-atom objects.
  unsigned bucket = offset / 8;
  unsigned bucketOffset = offset - bucket * 8;
  size_t typeSize = GetSize(type);
  switch (type) {
    case Type::I64:
    case Type::V64: {
      if (offset % 8 != 0) {
        llvm_unreachable("not implemented");
      } else {
        return (this->*mutate)(bucket, val);
      }
    }
    case Type::I8:
    case Type::I16:
    case Type::I32: {
      if  (bucketOffset + typeSize > 8) {
        llvm_unreachable("not implemented");
      } else {
        switch (val.GetKind()) {
          // If the incoming value is unknown, invalidate the whole bucket.
          case SymbolicValue::Kind::UNKNOWN:
          case SymbolicValue::Kind::UNKNOWN_INTEGER: {
            return (this->*mutate)(bucket, val);
          }
          // Attempt to mix an integer into the bucket.
          case SymbolicValue::Kind::INTEGER: {
            const auto &orig = buckets_[bucket];
            switch (orig.GetKind()) {
              case SymbolicValue::Kind::UNKNOWN:
              case SymbolicValue::Kind::UNKNOWN_INTEGER: {
                return (this->*mutate)(bucket, SymbolicValue::UnknownInteger());
              }
              case SymbolicValue::Kind::POINTER: {
                llvm_unreachable("not implemented");
              }
              case SymbolicValue::Kind::INTEGER: {
                APInt value = orig.GetInteger();
                value.insertBits(val.GetInteger(), bucketOffset * 8);
                return (this->*mutate)(bucket, SymbolicValue::Integer(value));
              }
            }
            llvm_unreachable("invalid bucket kind");
          }
          // TODO
          case SymbolicValue::Kind::POINTER: {
            llvm_unreachable("not implemented");
          }
        }
        llvm_unreachable("invalid value kind");
      }
    }
    case Type::I128:
    case Type::F32:
    case Type::F64:
    case Type::F80:
    case Type::F128: {
      llvm_unreachable("not implemented");
    }
  }
  llvm_unreachable("invalid type");
}

// -----------------------------------------------------------------------------
SymbolicValue SymbolicObject::ReadPrecise(int64_t offset, Type type)
{// This only works for single-atom objects.
  unsigned bucket = offset / 8;
  unsigned bucketOffset = offset - bucket * 8;
  size_t typeSize = GetSize(type);
  switch (type) {
    case Type::I64:
    case Type::V64: {
      if (offset % 8 != 0) {
        llvm_unreachable("not implemented");
      } else {
        return buckets_[bucket];
      }
    }
    case Type::I8:
    case Type::I16:
    case Type::I32: {
      if  (bucketOffset + typeSize > 8) {
        llvm_unreachable("not implemented");
      } else {
        const auto &orig = buckets_[bucket];
        switch (orig.GetKind()) {
          case SymbolicValue::Kind::UNKNOWN:
          case SymbolicValue::Kind::UNKNOWN_INTEGER: {
            return orig;
          }
          case SymbolicValue::Kind::POINTER: {
            llvm_unreachable("not implemented");
          }
          case SymbolicValue::Kind::INTEGER: {
            return SymbolicValue::Integer(orig.GetInteger().extractBits(
                typeSize * 8,
                bucketOffset * 8
            ));
          }
        }
        llvm_unreachable("invalid bucket kind");
      }
    }
    case Type::I128:
    case Type::F32:
    case Type::F64:
    case Type::F80:
    case Type::F128: {
      llvm_unreachable("not implemented");
    }
  }
  llvm_unreachable("invalid type");
}

// -----------------------------------------------------------------------------
bool SymbolicObject::Set(unsigned bucket, const SymbolicValue &val)
{
  if (val != buckets_[bucket]) {
    buckets_[bucket] = val;
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
bool SymbolicObject::Merge(unsigned bucket, const SymbolicValue &val)
{
  if (val != buckets_[bucket]) {
    buckets_[bucket] = val.LUB(buckets_[bucket]);
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
SymbolicDataObject::SymbolicDataObject(Object &object)
  : SymbolicObject(object.begin()->GetAlignment().value_or(llvm::Align(1)))
  , object_(object)
{
  if (object.size() == 1) {
    Atom &atom = *object.begin();
    LLVM_DEBUG(llvm::dbgs() << "\nBuilding object:\n\n" << atom << "\n");
    start_.emplace(&atom, std::make_pair(0u, 0u));
    size_ = atom.GetByteSize();
    for (auto it = atom.begin(); it != atom.end(); ) {
      Item *item = &*it++;
      switch (item->GetKind()) {
        case Item::Kind::INT8:
        case Item::Kind::INT16:
        case Item::Kind::INT32: {
          llvm_unreachable("not implemented");
        }
        case Item::Kind::INT64: {
          buckets_.push_back(SymbolicValue::Integer(
              llvm::APInt(64, item->GetInt64(), true)
          ));
          continue;
        }
        case Item::Kind::EXPR: {
          llvm_unreachable("not implemented");
        }
        case Item::Kind::FLOAT64: {
          llvm_unreachable("not implemented");
        }
        case Item::Kind::SPACE: {
          unsigned n = item->GetSpace();
          unsigned i;
          for (i = 0; i + 8 <= n; i += 8) {
            buckets_.push_back(SymbolicValue::Integer(llvm::APInt(64, 0, true)));
          }
          if (i != n) {
            llvm_unreachable("not implemented");
          }
          continue;
        }
        case Item::Kind::STRING: {
          llvm_unreachable("not implemented");
        }
      }
      llvm_unreachable("invalid item kind");
    }
  } else {
    llvm_unreachable("not implemented");
  }
}

// -----------------------------------------------------------------------------
bool SymbolicDataObject::Store(
    Atom *a,
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tStoring " << val << ":" << type << " to "
      << a->getName() << " + " << offset << "\n\n";
  );
  return WritePrecise(offset, val, type);
}

// -----------------------------------------------------------------------------
SymbolicValue SymbolicDataObject::Load(Atom *a, int64_t offset, Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tLoading " << type << " from "
      << a->getName() << " + " << offset << "\n\n";
  );
  return ReadPrecise(offset, type);
}

// -----------------------------------------------------------------------------
bool SymbolicDataObject::StoreImprecise(
    Atom *a,
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tTainting " << type << ":" << val << " to "
      << a->getName() << " + " << offset << "\n\n";
  );
  return WriteImprecise(offset, val, type);
}

// -----------------------------------------------------------------------------
bool SymbolicDataObject::StoreImprecise(const SymbolicValue &val, Type type)
{
#ifndef NDEBUG
  LLVM_DEBUG(llvm::dbgs() << "\tTainting " << type << ":" << val << " to \n");
  for (Atom &atom : object_) {
    LLVM_DEBUG(llvm::dbgs() << "\t\t" << atom.getName() << "\n");
  }
#endif
  size_t typeSize = GetSize(type);
  bool changed = false;
  for (size_t i = 0; i + typeSize < size_; i += typeSize) {
    changed = WriteImprecise(i, val, type) || changed;
  }
  return changed;
}

// -----------------------------------------------------------------------------
SymbolicFrameObject::SymbolicFrameObject(
    SymbolicFrame &frame,
    unsigned object,
    size_t size,
    llvm::Align align)
  : SymbolicObject(align)
  , frame_(frame)
  , object_(object)
{
  size_ = size;
  for (unsigned i = 0, n = (size + 7) / 8; i < n; ++i) {
    buckets_.push_back(SymbolicValue::Unknown());
  }
}

// -----------------------------------------------------------------------------
bool SymbolicFrameObject::Store(
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tStoring " << type << ":" << val << " to "
      << frame_.GetFunc().getName() << ":" << object_
      << " + " << offset << "\n\n";
  );
  return WritePrecise(offset, val, type);
}

// -----------------------------------------------------------------------------
bool SymbolicFrameObject::StoreImprecise(
    int64_t offset,
    const SymbolicValue &val,
    Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tTainting " << type << ":" << val << " to "
      << frame_.GetFunc().getName() << ":" << object_
      << " + " << offset << "\n\n";
  );
  return WriteImprecise(offset, val, type);
}

// -----------------------------------------------------------------------------
bool SymbolicFrameObject::StoreImprecise(const SymbolicValue &val, Type type)
{
  LLVM_DEBUG(llvm::dbgs()
      << "\tTainting " << type << ":" << val << " in \n"
      << frame_.GetFunc().getName() << ":" << object_
  );

  size_t typeSize = GetSize(type);
  bool changed = false;
  for (size_t i = 0; i + typeSize < size_; i += typeSize) {
    changed = WriteImprecise(i, val, type) || changed;
  }
  return changed;
}