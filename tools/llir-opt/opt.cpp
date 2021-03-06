// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <iostream>
#include <cstdlib>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/Host.h>

#include "core/bitcode.h"
#include "core/pass_manager.h"
#include "core/pass_registry.h"
#include "core/printer.h"
#include "core/prog.h"
#include "core/util.h"
#include "core/target/x86.h"
#include "core/target/ppc.h"
#include "core/target/aarch64.h"
#include "core/target/riscv.h"
#include "emitter/aarch64/aarch64emitter.h"
#include "emitter/coq/coqemitter.h"
#include "emitter/ppc/ppcemitter.h"
#include "emitter/riscv/riscvemitter.h"
#include "emitter/x86/x86emitter.h"
#include "passes/atom_simplify.h"
#include "passes/bypass_phi.h"
#include "passes/caml_alloc_inliner.h"
#include "passes/caml_assign.h"
#include "passes/caml_global_simplify.h"
#include "passes/code_layout.h"
#include "passes/cond_simplify.h"
#include "passes/const_global.h"
#include "passes/dead_code_elim.h"
#include "passes/dead_data_elim.h"
#include "passes/dead_func_elim.h"
#include "passes/dead_store.h"
#include "passes/dedup_block.h"
#include "passes/dedup_const.h"
#include "passes/eliminate_select.h"
#include "passes/eliminate_tags.h"
#include "passes/global_forward.h"
#include "passes/inliner.h"
#include "passes/libc_simplify.h"
#include "passes/linearise.h"
#include "passes/link.h"
#include "passes/localize_select.h"
#include "passes/mem_to_reg.h"
#include "passes/merge_stores.h"
#include "passes/move_elim.h"
#include "passes/move_push.h"
#include "passes/object_split.h"
#include "passes/phi_taut.h"
#include "passes/peephole.h"
#include "passes/pre_eval.h"
#include "passes/pta.h"
#include "passes/sccp.h"
#include "passes/simplify_cfg.h"
#include "passes/simplify_trampoline.h"
#include "passes/specialise.h"
#include "passes/stack_object_elim.h"
#include "passes/store_to_load.h"
#include "passes/tail_rec_elim.h"
#include "passes/undef_elim.h"
#include "passes/unused_arg.h"
#include "passes/value_numbering.h"
#include "stats/alloc_size.h"

namespace cl = llvm::cl;
namespace sys = llvm::sys;



/**
 * Enumeration of output formats.
 */
enum class OutputType {
  OBJ,
  ASM,
  COQ,
  LLIR,
  LLBC,
};

// -----------------------------------------------------------------------------
static cl::opt<bool>
optVerbose("v", cl::desc("verbosity flag"), cl::Hidden);

static cl::opt<std::string>
optInput(cl::Positional, cl::desc("<input>"), cl::Required);

static cl::opt<std::string>
optOutput("o", cl::desc("output"), cl::init("-"));

static cl::opt<bool>
optTime("time", cl::desc("time passes"), cl::init(false));

static cl::opt<OptLevel>
optOptLevel(
  cl::desc("optimisation level:"),
  cl::values(
    clEnumValN(OptLevel::O0, "O0", "No optimizations"),
    clEnumValN(OptLevel::O1, "O1", "Simple optimisations"),
    clEnumValN(OptLevel::O2, "O2", "Aggressive optimisations"),
    clEnumValN(OptLevel::O3, "O3", "Slow optimisations"),
    clEnumValN(OptLevel::O4, "O4", "All optimisations"),
    clEnumValN(OptLevel::Os, "Os", "Optimise for size")
  ),
  cl::init(OptLevel::O0)
);

static cl::opt<std::string>
optTriple("triple", cl::desc("Override host target triple"));

static cl::opt<std::string>
optCPU("mcpu", cl::desc("Override the host CPU"));

static cl::opt<std::string>
optTuneCPU("mtune", cl::desc("Override the tune CPU"));

static cl::opt<std::string>
optFS("mfs", cl::desc("Override the target features"));

static cl::opt<std::string>
optABI("mabi", cl::desc("Override the ABI"));

static cl::list<std::string>
optPasses("pass", cl::desc("specify a list of passes to run"));

static cl::opt<OutputType>
optEmit("emit", cl::desc("Emit text-based LLIR"),
  cl::values(
    clEnumValN(OutputType::OBJ,  "obj",  "target-specific object file"),
    clEnumValN(OutputType::ASM,  "asm",  "x86 object file"),
    clEnumValN(OutputType::COQ,  "coq",  "Coq IR"),
    clEnumValN(OutputType::LLIR, "llir", "LLIR text file"),
    clEnumValN(OutputType::LLBC, "llbc", "LLIR binary file")
  ),
  cl::Optional
);

static cl::opt<bool>
optShared("shared", cl::desc("Compile for a shared library"), cl::init(false));

static cl::opt<bool>
optStatic("static", cl::desc("Compile for a static binary"), cl::init(false));

static cl::opt<std::string>
optEntry("entry", cl::desc("Entry point of the application"));

static cl::opt<bool>
optVerify("verify", cl::desc("Enable the verified pass"), cl::init(false));

static cl::opt<std::string>
optSaveBefore("save-before", cl::desc("save IR to file before all passes"));



// -----------------------------------------------------------------------------
static void AddOpt0(PassManager &mngr)
{
}

// -----------------------------------------------------------------------------
static void AddOpt1(PassManager &mngr)
{
  mngr.Add<LinkPass>();
  // Initial simplification.
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<MoveElimPass>();
  mngr.Add<SimplifyCfgPass>();
  mngr.Add<TailRecElimPass>();
  mngr.Add<CamlAssignPass>();
  // General simplification.
  mngr.Group
    < ConstGlobalPass
    , SCCPPass
    , SimplifyCfgPass
    , DeadCodeElimPass
    , DeadFuncElimPass
    , DeadDataElimPass
    , DedupBlockPass
    , UnusedArgPass
    >();
  // Final transformation.
  mngr.Add<MergeStoresPass>();
  mngr.Add<StackObjectElimPass>();
  mngr.Add<LocalizeSelectPass>();
  mngr.Add<CamlAllocInlinerPass>();
}

// -----------------------------------------------------------------------------
static void AddOpt2(PassManager &mngr)
{
  mngr.Add<LinkPass>();
  // Initial simplification.
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<MoveElimPass>();
  mngr.Add<SimplifyCfgPass>();
  mngr.Add<TailRecElimPass>();
  mngr.Add<CamlAssignPass>();
  // General simplification.
  mngr.Group
    < ConstGlobalPass
    , SCCPPass
    , LibCSimplifyPass
    , SimplifyCfgPass
    , DedupConstPass
    , BypassPhiPass
    , SpecialisePass
    , DeadCodeElimPass
    , DeadFuncElimPass
    , DeadDataElimPass
    , MoveElimPass
    , MovePushPass
    , PhiTautPass
    , InlinerPass
    , CondSimplifyPass
    , DedupBlockPass
    , UnusedArgPass
    >();
  // Final transformation.
  mngr.Add<MergeStoresPass>();
  mngr.Add<StackObjectElimPass>();
  mngr.Add<LocalizeSelectPass>();
  mngr.Add<CamlAllocInlinerPass>();
}

// -----------------------------------------------------------------------------
static void AddOpt3(PassManager &mngr)
{
  mngr.Add<LinkPass>();
  // Initial simplification.
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<MoveElimPass>();
  mngr.Add<SimplifyCfgPass>();
  mngr.Add<TailRecElimPass>();
  mngr.Add<CamlAssignPass>();
  // General simplification.
  mngr.Group
    < ConstGlobalPass
    , SCCPPass
    , LibCSimplifyPass
    , SimplifyCfgPass
    , DedupConstPass
    , BypassPhiPass
    , SpecialisePass
    , DeadCodeElimPass
    , DeadFuncElimPass
    , DeadDataElimPass
    , MoveElimPass
    , MovePushPass
    , PhiTautPass
    , InlinerPass
    , CondSimplifyPass
    , DedupBlockPass
    , UnusedArgPass
    >();
  // Final transformation.
  mngr.Add<MergeStoresPass>();
  mngr.Add<StackObjectElimPass>();
  mngr.Add<LocalizeSelectPass>();
  mngr.Add<CamlAllocInlinerPass>();
}

// -----------------------------------------------------------------------------
static void AddOpt4(PassManager &mngr)
{
  mngr.Add<LinkPass>();
  // Initial simplification.
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<MoveElimPass>();
  mngr.Add<SimplifyCfgPass>();
  mngr.Add<TailRecElimPass>();
  mngr.Add<CamlAssignPass>();
  // General simplification.
  mngr.Group
    < PeepholePass
    , CamlGlobalSimplifyPass
    , ConstGlobalPass
    , LibCSimplifyPass
    , SCCPPass
    , DedupConstPass
    , SimplifyCfgPass
    , BypassPhiPass
    , SpecialisePass
    , EliminateSelectPass
    , DeadCodeElimPass
    , DeadFuncElimPass
    , DeadDataElimPass
    , MoveElimPass
    , MovePushPass
    , PhiTautPass
    , InlinerPass
    , CondSimplifyPass
    , DedupBlockPass
    , UnusedArgPass
    >();
  // Final transformation.
  mngr.Add<MergeStoresPass>();
  mngr.Add<StackObjectElimPass>();
  mngr.Add<LocalizeSelectPass>();
  mngr.Add<CodeLayoutPass>();
  mngr.Add<CamlAllocInlinerPass>();
}

// -----------------------------------------------------------------------------
static void AddOptS(PassManager &mngr)
{
  // First round - compact
  mngr.Add<LinkPass>();
  // Simplify functions and eliminate trivial items.
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<MoveElimPass>();
  mngr.Add<MovePushPass>();
  mngr.Add<SimplifyCfgPass>();
  mngr.Add<TailRecElimPass>();
  mngr.Add<SimplifyTrampolinePass>();
  mngr.Group<DeadFuncElimPass, DeadDataElimPass>();
  mngr.Add<DeadCodeElimPass>();
  mngr.Add<AtomSimplifyPass>();
  mngr.Add<CamlAssignPass>();
  // Optimise, evaluate and optimise again.
  mngr.Group
    < PeepholePass
    , CamlGlobalSimplifyPass
    , ConstGlobalPass
    , LibCSimplifyPass
    , ValueNumberingPass
    , SCCPPass
    , DedupBlockPass
    , SimplifyCfgPass
    , DedupConstPass
    , BypassPhiPass
    , DeadCodeElimPass
    , DeadFuncElimPass
    , DeadDataElimPass
    , MoveElimPass
    , MovePushPass
    , PhiTautPass
    , EliminateSelectPass
    , EliminateTagsPass
    , SpecialisePass
    , InlinerPass
    , CondSimplifyPass
    , ObjectSplitPass
    , StoreToLoadPass
    , DeadStorePass
    , MemoryToRegisterPass
    , UnusedArgPass
    , GlobalForwardPass
    >();
  // Final simplification.
  mngr.Add<MergeStoresPass>();
  mngr.Add<LocalizeSelectPass>();
  mngr.Add<CodeLayoutPass>();
  mngr.Add<StackObjectElimPass>();
}

// -----------------------------------------------------------------------------
static std::unique_ptr<Target>
GetTarget(
    const llvm::Triple &tt,
    const std::string &cpu,
    const std::string &tuneCPU,
    const std::string &fs,
    const std::string &abi,
    bool shared)
{
  switch (tt.getArch()) {
    case llvm::Triple::x86:
    case llvm::Triple::x86_64: {
      return std::make_unique<X86Target>(tt, cpu, tuneCPU, fs, abi, shared);
    }
    case llvm::Triple::aarch64: {
      return std::make_unique<AArch64Target>(tt, cpu, tuneCPU, fs, abi, shared);
    }
    case llvm::Triple::riscv64: {
      return std::make_unique<RISCVTarget>(tt, cpu, tuneCPU, fs, abi, shared);
    }
    case llvm::Triple::ppc64le: {
      return std::make_unique<PPCTarget>(tt, cpu, tuneCPU, fs, abi, shared);
    }
    default: {
      return nullptr;
    }
  }
}

// -----------------------------------------------------------------------------
int main(int argc, char **argv)
{
  // Initialise the relevant LLVM modules.
  llvm::InitLLVM X(argc, argv);
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // Parse command line options.
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "LLIR optimiser\n")) {
    return EXIT_FAILURE;
  }

  // Find the host triple.
  llvm::Triple hostTriple(llvm::sys::getDefaultTargetTriple());

  // Get the target triple to compile for.
  llvm::Triple triple;
  if (!optTriple.empty()) {
    triple = llvm::Triple(optTriple);
  } else {
    auto target = ParseToolName(argc ? argv[0] : "opt", "opt");
    if (!target.empty()) {
      triple = llvm::Triple(target).getNativeVariant();
    } else {
      triple = hostTriple;
    }
  }
  // Find the CPU to compile for.
  std::string CPU;
  if (optCPU.empty() && triple.getArch() == hostTriple.getArch()) {
    CPU = std::string(llvm::sys::getHostCPUName());
  } else {
    CPU = optCPU;
  }
  // Process the tune argument.
  std::string tuneCPU = optTuneCPU.empty() ? CPU : optTuneCPU;

  // Find the target architecture.
  auto t = GetTarget(triple, CPU, tuneCPU, optFS, optABI, optShared);
  if (!t) {
    llvm::errs() << "[Error] Cannot find target: " + triple.str() + "\n";
    return EXIT_FAILURE;
  }

  // Open the input.
  auto FileOrErr = llvm::MemoryBuffer::getFileOrSTDIN(optInput);
  if (auto EC = FileOrErr.getError()) {
    llvm::errs() << "[Error] Cannot open input: " + EC.message();
    return EXIT_FAILURE;
  }

  // Parse the linked blob: if file starts with magic, parse bitcode.
  auto buffer = FileOrErr.get()->getMemBufferRef().getBuffer();
  std::unique_ptr<Prog> prog(Parse(buffer, Abspath(optInput)));
  if (!prog) {
    return EXIT_FAILURE;
  }

  // Register all the passes.
  PassRegistry registry;
  registry.Register<AllocSizePass>();
  registry.Register<CamlAllocInlinerPass>();
  registry.Register<CamlGlobalSimplifyPass>();
  registry.Register<CamlAssignPass>();
  registry.Register<DeadCodeElimPass>();
  registry.Register<DeadDataElimPass>();
  registry.Register<DeadFuncElimPass>();
  registry.Register<DeadStorePass>();
  registry.Register<DedupBlockPass>();
  registry.Register<SpecialisePass>();
  registry.Register<InlinerPass>();
  registry.Register<LinkPass>();
  registry.Register<MoveElimPass>();
  registry.Register<MovePushPass>();
  registry.Register<PreEvalPass>();
  registry.Register<SCCPPass>();
  registry.Register<SimplifyCfgPass>();
  registry.Register<SimplifyTrampolinePass>();
  registry.Register<StackObjectElimPass>();
  registry.Register<TailRecElimPass>();
  registry.Register<ConstGlobalPass>();
  registry.Register<UndefElimPass>();
  registry.Register<MemoryToRegisterPass>();
  registry.Register<PointsToAnalysis>();
  registry.Register<AtomSimplifyPass>();
  registry.Register<EliminateSelectPass>();
  registry.Register<CondSimplifyPass>();
  registry.Register<StoreToLoadPass>();
  registry.Register<LibCSimplifyPass>();
  registry.Register<UnusedArgPass>();
  registry.Register<GlobalForwardPass>();
  registry.Register<ObjectSplitPass>();
  registry.Register<ValueNumberingPass>();
  registry.Register<LinearisePass>();
  registry.Register<PhiTautPass>();
  registry.Register<CodeLayoutPass>();
  registry.Register<LocalizeSelectPass>();
  registry.Register<EliminateTagsPass>();

  // Set up the pipeline.
  PassConfig cfg(optOptLevel, optStatic, optShared, optEntry);
  PassManager passMngr(cfg, t.get(), optSaveBefore, optVerbose, optTime, optVerify);
  if (!optPasses.empty()) {
    for (auto &passName : optPasses) {
      registry.Add(passMngr, std::string(passName));
    }
  } else {
    switch (optOptLevel) {
      case OptLevel::O0: AddOpt0(passMngr); break;
      case OptLevel::O1: AddOpt1(passMngr); break;
      case OptLevel::O2: AddOpt2(passMngr); break;
      case OptLevel::O3: AddOpt3(passMngr); break;
      case OptLevel::O4: AddOpt4(passMngr); break;
      case OptLevel::Os: AddOptS(passMngr); break;
    }
  }

  // Determine the output type.
  llvm::StringRef out = optOutput;

  // Figure out the output type.
  OutputType type;
  if (optEmit.getNumOccurrences()) {
    type = optEmit;
  } else if (out.endswith(".llir")) {
    type = OutputType::LLIR;
  } else if (out.endswith(".llbc")) {
    type = OutputType::LLBC;
  } else if (out.endswith(".S") || out.endswith(".s") || out == "-") {
    type = OutputType::ASM;
  } else if (out.endswith(".o")) {
    type = OutputType::OBJ;
  } else if (out.endswith(".v")) {
    type = OutputType::COQ;
  } else {
    llvm::errs() << "[Error] Unknown output format\n";
    return EXIT_FAILURE;
  }

  // Check if output is binary.
  // Add DCE and move elimination if code is generatoed.
  bool isBinary = false;
  switch (type) {
    case OutputType::ASM: {
      passMngr.Add<MoveElimPass>();
      passMngr.Add<DeadCodeElimPass>();
      isBinary = false;
      break;
    }
    case OutputType::OBJ: {
      passMngr.Add<MoveElimPass>();
      passMngr.Add<DeadCodeElimPass>();
      isBinary = true;
      break;
    }
    case OutputType::LLIR: {
      isBinary = false;
      break;
    }
    case OutputType::LLBC: {
      isBinary = true;
      break;
    }
    case OutputType::COQ: {
      isBinary = false;
      break;
    }
  }

  // Run the optimiser.
  passMngr.Run(*prog);

  // Open the output stream.
  std::error_code err;
  sys::fs::OpenFlags fs = isBinary ? sys::fs::F_None : sys::fs::F_Text;
  auto output = std::make_unique<llvm::ToolOutputFile>(optOutput, err, fs);
  if (err) {
    llvm::errs() << err.message() << "\n";
    return EXIT_FAILURE;
  }

  // Helper to create an emitter.
  auto getEmitter = [&] () -> std::unique_ptr<Emitter> {
    auto &os = output->os();
    switch (triple.getArch()) {
      case llvm::Triple::x86:
      case llvm::Triple::x86_64: {
        return std::make_unique<X86Emitter>(optInput, os, *t->As<X86Target>());
      }
      case llvm::Triple::aarch64: {
        return std::make_unique<AArch64Emitter>(optInput, os, *t->As<AArch64Target>());
      }
      case llvm::Triple::riscv64: {
        return std::make_unique<RISCVEmitter>(optInput, os, *t->As<RISCVTarget>());
      }
      case llvm::Triple::ppc64le: {
        return std::make_unique<PPCEmitter>(optInput, os, *t->As<PPCTarget>());
      }
      default: {
        llvm::report_fatal_error("Unknown architecture: " + triple.normalize());
      }
    }
  };

  // Generate code.
  switch (type) {
    case OutputType::ASM: {
      getEmitter()->EmitASM(*prog);
      break;
    }
    case OutputType::OBJ: {
      getEmitter()->EmitOBJ(*prog);
      break;
    }
    case OutputType::LLIR: {
      Printer(output->os()).Print(*prog);
      break;
    }
    case OutputType::LLBC: {
      BitcodeWriter(output->os()).Write(*prog);
      break;
    }
    case OutputType::COQ: {
      CoqEmitter(output->os()).Write(*prog);
      break;
    }
  }

  output->keep();
  return EXIT_SUCCESS;
}
