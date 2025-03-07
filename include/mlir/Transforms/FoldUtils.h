//===- FoldUtils.h - Operation Fold Utilities -------------------*- C++ -*-===//
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
// This header file declares various operation folding utilities. These
// utilities are intended to be used by passes to unify and simply their logic.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_TRANSFORMS_FOLDUTILS_H
#define MLIR_TRANSFORMS_FOLDUTILS_H

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dialect.h"

namespace mlir {
class FuncOp;
using Function = FuncOp;
class Operation;
class Value;

/// A utility class for folding operations, and unifying duplicated constants
/// generated along the way.
class OperationFolder {
public:
  /// Tries to perform folding on the given `op`, including unifying
  /// deduplicated constants. If successful, replaces `op`'s uses with
  /// folded results, and returns success. `preReplaceAction` is invoked on `op`
  /// before it is replaced. 'processGeneratedConstants' is invoked for any new
  /// operations generated when folding. If the op was completely folded it is
  /// erased.
  LogicalResult tryToFold(
      Operation *op,
      llvm::function_ref<void(Operation *)> processGeneratedConstants = nullptr,
      llvm::function_ref<void(Operation *)> preReplaceAction = nullptr);

  /// Notifies that the given constant `op` should be remove from this
  /// OperationFolder's internal bookkeeping.
  ///
  /// Note: this method must be called if a constant op is to be deleted
  /// externally to this OperationFolder. `op` must be a constant op.
  void notifyRemoval(Operation *op);

  /// Create an operation of specific op type with the given builder,
  /// and immediately try to fold it. This function populates 'results' with
  /// the results after folding the operation.
  template <typename OpTy, typename... Args>
  void create(OpBuilder &builder, SmallVectorImpl<Value *> &results,
              Location location, Args &&... args) {
    Operation *op = builder.create<OpTy>(location, std::forward<Args>(args)...);
    if (failed(tryToFold(op, results)))
      results.assign(op->result_begin(), op->result_end());
    else if (op->getNumResults() != 0)
      op->erase();
  }

  /// Overload to create or fold a single result operation.
  template <typename OpTy, typename... Args>
  typename std::enable_if<OpTy::template hasTrait<OpTrait::OneResult>(),
                          Value *>::type
  create(OpBuilder &builder, Location location, Args &&... args) {
    SmallVector<Value *, 1> results;
    create<OpTy>(builder, results, location, std::forward<Args>(args)...);
    return results.front();
  }

  /// Overload to create or fold a zero result operation.
  template <typename OpTy, typename... Args>
  typename std::enable_if<OpTy::template hasTrait<OpTrait::ZeroResult>(),
                          OpTy>::type
  create(OpBuilder &builder, Location location, Args &&... args) {
    auto op = builder.create<OpTy>(location, std::forward<Args>(args)...);
    SmallVector<Value *, 0> unused;
    (void)tryToFold(op.getOperation(), unused);

    // Folding cannot remove a zero-result operation, so for convenience we
    // continue to return it.
    return op;
  }

private:
  /// This map keeps track of uniqued constants by dialect, attribute, and type.
  /// A constant operation materializes an attribute with a type. Dialects may
  /// generate different constants with the same input attribute and type, so we
  /// also need to track per-dialect.
  using ConstantMap =
      DenseMap<std::tuple<Dialect *, Attribute, Type>, Operation *>;

  /// Tries to perform folding on the given `op`. If successful, populates
  /// `results` with the results of the folding.
  LogicalResult tryToFold(Operation *op, SmallVectorImpl<Value *> &results,
                          llvm::function_ref<void(Operation *)>
                              processGeneratedConstants = nullptr);

  /// Try to get or create a new constant entry. On success this returns the
  /// constant operation, nullptr otherwise.
  Operation *tryGetOrCreateConstant(ConstantMap &uniquedConstants,
                                    Dialect *dialect, OpBuilder &builder,
                                    Attribute value, Type type, Location loc);

  /// A mapping between an insertion region and the constants that have been
  /// created within it.
  DenseMap<Region *, ConstantMap> foldScopes;

  /// This map tracks all of the dialects that an operation is referenced by;
  /// given that many dialects may generate the same constant.
  DenseMap<Operation *, SmallVector<Dialect *, 2>> referencedDialects;
};

} // end namespace mlir

#endif // MLIR_TRANSFORMS_FOLDUTILS_H
