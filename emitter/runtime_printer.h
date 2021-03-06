// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <llvm/Pass.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/Target/TargetMachine.h>

class Prog;
class Data;



/**
 * Pass to print runtime methods to the output object.
 */
class RuntimePrinter : public llvm::ModulePass {
public:
  /// Initialises the pass which prints data sections.
  RuntimePrinter(
      char &ID,
      const Prog &Prog,
      const llvm::TargetMachine &tm,
      llvm::MCContext &ctx,
      llvm::MCStreamer &os,
      const llvm::MCObjectFileInfo &objInfo,
      bool shared
  );

private:
  /// Creates MachineFunctions from LLIR IR.
  bool runOnModule(llvm::Module &M) override;
  /// Requires MachineModuleInfo.
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

protected:
  /// Emits caml_call_gc
  virtual void EmitCamlCallGc(llvm::Function &F) = 0;
  /// Emits caml_c_call
  virtual void EmitCamlCCall(llvm::Function &F) = 0;

protected:
  /// Program to print.
  const Prog &prog_;
  /// Reference to the target machine.
  const llvm::TargetMachine &tm_;
  /// LLVM context.
  llvm::MCContext &ctx_;
  /// Streamer to emit output to.
  llvm::MCStreamer &os_;
  /// Object-file specific information.
  const llvm::MCObjectFileInfo &objInfo_;
  /// Data layout.
  const llvm::DataLayout layout_;
  /// Flag to indicate whether a shared library or a static library is built.
  bool shared_;
};
