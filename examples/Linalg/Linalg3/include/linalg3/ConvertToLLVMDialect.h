//===- ConvertToLLVMDialect.h - conversion from Linalg to LLVM --*- C++ -*-===//
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

#ifndef LINALG3_CONVERTTOLLVMDIALECT_H_
#define LINALG3_CONVERTTOLLVMDIALECT_H_

namespace mlir {
class ModuleOp;
using Module = ModuleOp;
} // end namespace mlir

namespace linalg {
void convertLinalg3ToLLVM(mlir::Module module);
} // end namespace linalg

#endif // LINALG3_CONVERTTOLLVMDIALECT_H_
