// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <cstdlib>

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SCCIterator.h>

#include "core/block.h"
#include "core/cast.h"
#include "core/call_graph.h"
#include "core/cfg.h"
#include "core/constant.h"
#include "core/data.h"
#include "core/func.h"
#include "core/insts.h"
#include "core/prog.h"
#include "passes/vtpta/builder.h"
#include "passes/vtpta/constraint.h"
#include "passes/vtpta.h"



// -----------------------------------------------------------------------------
const char *VariantTypePointsToAnalysis::kPassID = "vtpta";

// -----------------------------------------------------------------------------
bool VariantTypePointsToAnalysis::Run(Prog &prog)
{
  CallGraph graph(prog);

  /*
  for (auto it = llvm::scc_begin(&graph); !it.isAtEnd(); ++it) {
    const std::vector<const CallGraph::Node *> &scc = *it;
    // TODO: type everything in the SCC.
    for (auto *node : scc) {
      if (auto *f = node->GetCaller()) {
        if (f->GetCallingConv() != CallingConv::CAML) {
          continue;
        }
        if (f->size() != 3) {
          continue;
        }
        if (f->begin()->size() > 10) {
          continue;
        }
        p.Print(*f);
      }
    }
  }
  */
  llvm_unreachable("not implemented");
}

// -----------------------------------------------------------------------------
const char *VariantTypePointsToAnalysis::GetPassName() const
{
  return "Variant Type Points-To Analysis";
}

// -----------------------------------------------------------------------------
char AnalysisID<VariantTypePointsToAnalysis>::ID;
