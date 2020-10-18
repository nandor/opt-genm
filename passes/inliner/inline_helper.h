// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <llvm/ADT/PostOrderIterator.h>

#include "core/block.h"
#include "core/cast.h"
#include "core/clone.h"
#include "core/func.h"
#include "core/cfg.h"

class TrampolineGraph;



/**
 * Inline clone helper
 */
class InlineHelper final : public CloneVisitor {
public:
  /**
   * Initialises the inliner.
   *
   * @param call    Call site to inline into
   * @param callee  Callee to inline into the call site.
   * @param graph   OCaml trampoline graph.
   */
  InlineHelper(CallSite *call, Func *callee, TrampolineGraph &graph);

  /// Inlines the function.
  void Inline();

private:
  /// Creates a copy of an instruction.
  Inst *Duplicate(Block *block, Inst *inst);

  /// Maps a block.
  Block *Map(Block *block) override { return blocks_[block]; }
  /// Maps an instruction.
  Inst *Map(Inst *inst) override { return insts_[inst]; }

  /// Inlines annotations.
  AnnotSet Annot(const Inst *inst) override;

  /// Extends a value from one type to another.
  Inst *Convert(Type argType, Type valType, Inst *valInst, AnnotSet &&annot);

  /// Duplicates blocks from the source function.
  void DuplicateBlocks();
  /// Split the entry.
  void SplitEntry();

private:
  /// Flag indicating if the call is a tail call.
  const bool isTailCall_;
  /// Return type of the call.
  const std::vector<Type> types_;
  /// Call site being inlined.
  Inst *call_;
  /// Call argument.
  Inst *callCallee_;
  /// Annotations of the original call.
  const AnnotSet callAnnot_;
  /// Entry block.
  Block *entry_;
  /// Called function.
  Func *callee_;
  /// Caller function.
  Func *caller_;
  /// Mapping from callee to caller frame indices.
  llvm::DenseMap<unsigned, unsigned> frameIndices_;
  /// Exit block.
  Block *exit_;
  /// Final PHI.
  std::vector<PhiInst *> phis_;
  /// Number of exit nodes.
  unsigned numExits_;
  /// Arguments.
  llvm::SmallVector<Inst *, 8> args_;
  /// Mapping from old to new blocks.
  llvm::DenseMap<Block *, Block *> blocks_;
  /// Map of cloned instructions.
  std::unordered_map<Inst *, Inst *> insts_;
  /// Block order.
  llvm::ReversePostOrderTraversal<Func *> rpot_;
  /// Graph which determines calls needing trampolines.
  TrampolineGraph &graph_;
};

