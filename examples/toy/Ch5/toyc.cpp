//===- toyc.cpp - The Toy Compiler ----------------------------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements the entry point for the Toy compiler.
//
//===----------------------------------------------------------------------===//

#include "toy/Dialect.h"
#include "toy/Lowering.h"
#include "toy/MLIRGen.h"
#include "toy/Parser.h"
#include "toy/Passes.h"

#include "linalg1/Dialect.h"
#include "mlir/Analysis/Verifier.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;
namespace cl = llvm::cl;

static cl::opt<std::string> inputFilename(cl::Positional,
                                          cl::desc("<input toy file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

namespace {
enum InputType { Toy, MLIR };
}
static cl::opt<enum InputType> inputType(
    "x", cl::init(Toy), cl::desc("Decided the kind of output desired"),
    cl::values(clEnumValN(Toy, "toy", "load the input file as a Toy source.")),
    cl::values(clEnumValN(MLIR, "mlir",
                          "load the input file as an MLIR file")));

namespace {
enum Action {
  None,
  DumpAST,
  DumpMLIR,
  DumpMLIRLinalg,
  DumpLLVMDialect,
  DumpLLVMIR,
  RunJIT
};
}
static cl::opt<enum Action> emitAction(
    "emit", cl::desc("Select the kind of output desired"),
    cl::values(clEnumValN(DumpAST, "ast", "output the AST dump")),
    cl::values(clEnumValN(DumpMLIR, "mlir", "output the MLIR dump")),
    cl::values(clEnumValN(DumpMLIRLinalg, "mlir-linalg",
                          "output the MLIR dump after linalg lowering")),
    cl::values(clEnumValN(DumpLLVMDialect, "llvm-dialect",
                          "output the LLVM MLIR Dialect dump")),
    cl::values(clEnumValN(DumpLLVMIR, "llvm-ir", "output the LLVM IR dump")),
    cl::values(
        clEnumValN(RunJIT, "jit",
                   "JIT the code and run it by invoking the main function")));

static cl::opt<bool> EnableOpt("opt", cl::desc("Enable optimizations"));

/// Returns a Toy AST resulting from parsing the file or a nullptr on error.
std::unique_ptr<toy::ModuleAST> parseInputFile(llvm::StringRef filename) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileOrErr =
      llvm::MemoryBuffer::getFileOrSTDIN(filename);
  if (std::error_code EC = FileOrErr.getError()) {
    llvm::errs() << "Could not open input file: " << EC.message() << "\n";
    return nullptr;
  }
  auto buffer = FileOrErr.get()->getBuffer();
  LexerBuffer lexer(buffer.begin(), buffer.end(), filename);
  Parser parser(lexer);
  return parser.ParseModule();
}

mlir::LogicalResult optimize(mlir::Module module) {
  mlir::PassManager pm;
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(createShapeInferencePass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());

  // Apply any generic pass manager command line options.
  applyPassManagerCLOptions(pm);

  return pm.run(module);
}

mlir::LogicalResult lowerDialect(mlir::Module module, bool OnlyLinalg) {
  mlir::PassManager pm;
  pm.addPass(createEarlyLoweringPass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());
  if (!OnlyLinalg) {
    pm.addPass(createLateLoweringPass());
    pm.addPass(mlir::createCanonicalizerPass());
    pm.addPass(mlir::createCSEPass());
  }
  // Apply any generic pass manager command line options.
  applyPassManagerCLOptions(pm);

  return pm.run(module);
}

mlir::OwningModuleRef loadFileAndProcessModule(
    mlir::MLIRContext &context, bool EnableLinalgLowering = false,
    bool EnableLLVMLowering = false, bool EnableOpt = false) {

  mlir::OwningModuleRef module;
  if (inputType == InputType::MLIR ||
      llvm::StringRef(inputFilename).endswith(".mlir")) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(inputFilename);
    if (std::error_code EC = fileOrErr.getError()) {
      llvm::errs() << "Could not open input file: " << EC.message() << "\n";
      return nullptr;
    }
    llvm::SourceMgr sourceMgr;
    sourceMgr.AddNewSourceBuffer(std::move(*fileOrErr), llvm::SMLoc());
    module = mlir::parseSourceFile(sourceMgr, &context);
    if (!module) {
      llvm::errs() << "Error can't load file " << inputFilename << "\n";
      return nullptr;
    }
    if (failed(mlir::verify(*module))) {
      llvm::errs() << "Error verifying MLIR module\n";
      return nullptr;
    }
  } else {
    auto moduleAST = parseInputFile(inputFilename);
    module = mlirGen(context, *moduleAST);
  }
  if (!module)
    return nullptr;
  if (EnableOpt) {
    if (failed(optimize(*module))) {
      llvm::errs() << "Module optimization failed\n";
      return nullptr;
    }
  }
  if (EnableLLVMLowering || EnableLinalgLowering) {
    if (failed(lowerDialect(*module, !EnableLLVMLowering))) {
      llvm::errs() << "Module lowering failed\n";
      return nullptr;
    }
  }
  return module;
}

int dumpMLIR() {
  mlir::MLIRContext context;
  auto module =
      loadFileAndProcessModule(context, /*EnableLinalgLowering=*/false,
                               /*EnableLLVMLowering=*/false, EnableOpt);
  if (!module)
    return -1;
  module->dump();
  return 0;
}

int dumpMLIRLinalg() {
  mlir::MLIRContext context;
  auto module = loadFileAndProcessModule(context, /*EnableLinalgLowering=*/true,
                                         /*EnableLLVMLowering=*/false,
                                         /* EnableOpt=*/true);
  if (!module)
    return -1;
  module->dump();
  return 0;
}

int dumpLLVMDialect() {
  mlir::MLIRContext context;
  auto module = loadFileAndProcessModule(
      context, /*EnableLinalgLowering=*/false, /* EnableLLVMLowering=*/true,
      /* EnableOpt=*/true);
  if (!module) {
    llvm::errs() << "Failed to load/lower MLIR module\n";
    return -1;
  }
  module->dump();
  return 0;
}

int dumpLLVMIR() {
  mlir::MLIRContext context;
  auto module = loadFileAndProcessModule(
      context, /*EnableLinalgLowering=*/false, /* EnableLLVMLowering=*/true,
      /* EnableOpt=*/true);
  if (!module) {
    llvm::errs() << "Failed to load/lower MLIR module\n";
    return -1;
  }
  auto llvmModule = translateModuleToLLVMIR(*module);
  if (!llvmModule) {
    llvm::errs() << "Failed to emit LLVM IR\n";
    return -1;
  }
  // Initialize LLVM targets.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  mlir::ExecutionEngine::setupTargetTriple(llvmModule.get());
  auto optPipeline = mlir::makeOptimizingTransformer(
      /* optLevel=*/EnableOpt ? 3 : 0, /* sizeLevel=*/0);
  if (auto err = optPipeline(llvmModule.get())) {
    llvm::errs() << "Failed to optimize LLVM IR " << err << "\n";
    return -1;
  }
  llvm::errs() << *llvmModule << "\n";
  return 0;
}

int runJit() {
  mlir::MLIRContext context;
  auto module = loadFileAndProcessModule(
      context, /*EnableLinalgLowering=*/false, /* EnableLLVMLowering=*/true,
      /* EnableOpt=*/true);

  // Initialize LLVM targets.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  // Create an MLIR execution engine. The execution engine eagerly JIT-compiles
  // the module.
  auto optPipeline = mlir::makeOptimizingTransformer(
      /* optLevel=*/EnableOpt ? 3 : 0, /* sizeLevel=*/0);
  auto maybeEngine = mlir::ExecutionEngine::create(*module, optPipeline);
  assert(maybeEngine && "failed to construct an execution engine");
  auto &engine = maybeEngine.get();

  // Invoke the JIT-compiled function with the arguments.  Note that, for API
  // uniformity reasons, it takes a list of type-erased pointers to arguments.
  auto invocationResult = engine->invoke("main");
  if (invocationResult) {
    llvm::errs() << "JIT invocation failed\n";
    return -1;
  }

  return 0;
}

int dumpAST() {
  if (inputType == InputType::MLIR) {
    llvm::errs() << "Can't dump a Toy AST when the input is MLIR\n";
    return 5;
  }

  auto moduleAST = parseInputFile(inputFilename);
  if (!moduleAST)
    return 1;

  dump(*moduleAST);
  return 0;
}

int main(int argc, char **argv) {
  // Register our Dialects with MLIR
  mlir::registerDialect<ToyDialect>();
  mlir::registerDialect<linalg::LinalgDialect>();

  mlir::registerPassManagerCLOptions();
  cl::ParseCommandLineOptions(argc, argv, "toy compiler\n");

  switch (emitAction) {
  case Action::DumpAST:
    return dumpAST();
  case Action::DumpMLIR:
    return dumpMLIR();
  case Action::DumpMLIRLinalg:
    return dumpMLIRLinalg();
  case Action::DumpLLVMDialect:
    return dumpLLVMDialect();
  case Action::DumpLLVMIR:
    return dumpLLVMIR();
  case Action::RunJIT:
    return runJit();
  default:
    llvm::errs() << "No action specified (parsing only?), use -emit=<action>\n";
    return -1;
  }

  return 0;
}
