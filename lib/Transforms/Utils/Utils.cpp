//===- Utils.cpp ---- Misc utilities for code and data transformation -----===//
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
// This file implements miscellaneous transformation routines for non-loop IR
// structures.
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/Utils.h"

#include "mlir/AffineOps/AffineOps.h"
#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/Dominance.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Module.h"
#include "mlir/StandardOps/Ops.h"
#include "mlir/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"
using namespace mlir;

/// Return true if this operation dereferences one or more memref's.
// Temporary utility: will be replaced when this is modeled through
// side-effects/op traits. TODO(b/117228571)
static bool isMemRefDereferencingOp(Operation &op) {
  if (isa<AffineLoadOp>(op) || isa<AffineStoreOp>(op) ||
      isa<AffineDmaStartOp>(op) || isa<AffineDmaWaitOp>(op))
    return true;
  return false;
}

/// Return the AffineMapAttr associated with memory 'op' on 'memref'.
static NamedAttribute getAffineMapAttrForMemRef(Operation *op, Value *memref) {
  if (auto loadOp = dyn_cast<AffineLoadOp>(op))
    return loadOp.getAffineMapAttrForMemRef(memref);
  else if (auto storeOp = dyn_cast<AffineStoreOp>(op))
    return storeOp.getAffineMapAttrForMemRef(memref);
  else if (auto dmaStart = dyn_cast<AffineDmaStartOp>(op))
    return dmaStart.getAffineMapAttrForMemRef(memref);
  assert(isa<AffineDmaWaitOp>(op));
  return cast<AffineDmaWaitOp>(op).getAffineMapAttrForMemRef(memref);
}

bool mlir::replaceAllMemRefUsesWith(Value *oldMemRef, Value *newMemRef,
                                    ArrayRef<Value *> extraIndices,
                                    AffineMap indexRemap,
                                    ArrayRef<Value *> extraOperands,
                                    Operation *domInstFilter,
                                    Operation *postDomInstFilter) {
  unsigned newMemRefRank = newMemRef->getType().cast<MemRefType>().getRank();
  (void)newMemRefRank; // unused in opt mode
  unsigned oldMemRefRank = oldMemRef->getType().cast<MemRefType>().getRank();
  (void)newMemRefRank;
  if (indexRemap) {
    assert(indexRemap.getNumSymbols() == 0 && "pure dimensional map expected");
    assert(indexRemap.getNumInputs() == extraOperands.size() + oldMemRefRank);
    assert(indexRemap.getNumResults() + extraIndices.size() == newMemRefRank);
  } else {
    assert(oldMemRefRank + extraIndices.size() == newMemRefRank);
  }

  // Assert same elemental type.
  assert(oldMemRef->getType().cast<MemRefType>().getElementType() ==
         newMemRef->getType().cast<MemRefType>().getElementType());

  std::unique_ptr<DominanceInfo> domInfo;
  std::unique_ptr<PostDominanceInfo> postDomInfo;
  if (domInstFilter)
    domInfo = llvm::make_unique<DominanceInfo>(
        domInstFilter->getParentOfType<FuncOp>());

  if (postDomInstFilter)
    postDomInfo = llvm::make_unique<PostDominanceInfo>(
        postDomInstFilter->getParentOfType<FuncOp>());

  // The ops where memref replacement succeeds are replaced with new ones.
  SmallVector<Operation *, 8> opsToErase;

  // Walk all uses of old memref. Operation using the memref gets replaced.
  for (auto *opInst : llvm::make_early_inc_range(oldMemRef->getUsers())) {
    // Skip this use if it's not dominated by domInstFilter.
    if (domInstFilter && !domInfo->dominates(domInstFilter, opInst))
      continue;

    // Skip this use if it's not post-dominated by postDomInstFilter.
    if (postDomInstFilter &&
        !postDomInfo->postDominates(postDomInstFilter, opInst))
      continue;

    // Skip dealloc's - no replacement is necessary, and a replacement doesn't
    // hurt dealloc's.
    if (isa<DeallocOp>(opInst))
      continue;

    // Check if the memref was used in a non-deferencing context. It is fine for
    // the memref to be used in a non-deferencing way outside of the region
    // where this replacement is happening.
    if (!isMemRefDereferencingOp(*opInst))
      // Failure: memref used in a non-deferencing op (potentially escapes); no
      // replacement in these cases.
      return false;

    auto getMemRefOperandPos = [&]() -> unsigned {
      unsigned i, e;
      for (i = 0, e = opInst->getNumOperands(); i < e; i++) {
        if (opInst->getOperand(i) == oldMemRef)
          break;
      }
      assert(i < opInst->getNumOperands() && "operand guaranteed to be found");
      return i;
    };

    OpBuilder builder(opInst);
    unsigned memRefOperandPos = getMemRefOperandPos();
    NamedAttribute oldMapAttrPair =
        getAffineMapAttrForMemRef(opInst, oldMemRef);
    AffineMap oldMap = oldMapAttrPair.second.cast<AffineMapAttr>().getValue();
    unsigned oldMapNumInputs = oldMap.getNumInputs();
    SmallVector<Value *, 4> oldMapOperands(
        opInst->operand_begin() + memRefOperandPos + 1,
        opInst->operand_begin() + memRefOperandPos + 1 + oldMapNumInputs);
    SmallVector<Value *, 4> affineApplyOps;

    // Apply 'oldMemRefOperands = oldMap(oldMapOperands)'.
    SmallVector<Value *, 4> oldMemRefOperands;
    oldMemRefOperands.reserve(oldMemRefRank);
    if (oldMap != builder.getMultiDimIdentityMap(oldMap.getNumDims())) {
      for (auto resultExpr : oldMap.getResults()) {
        auto singleResMap = builder.getAffineMap(
            oldMap.getNumDims(), oldMap.getNumSymbols(), resultExpr);
        auto afOp = builder.create<AffineApplyOp>(opInst->getLoc(),
                                                  singleResMap, oldMapOperands);
        oldMemRefOperands.push_back(afOp);
        affineApplyOps.push_back(afOp);
      }
    } else {
      oldMemRefOperands.append(oldMapOperands.begin(), oldMapOperands.end());
    }

    // Construct new indices as a remap of the old ones if a remapping has been
    // provided. The indices of a memref come right after it, i.e.,
    // at position memRefOperandPos + 1.
    SmallVector<Value *, 4> remapOperands;
    remapOperands.reserve(extraOperands.size() + oldMemRefRank);
    remapOperands.append(extraOperands.begin(), extraOperands.end());
    remapOperands.append(oldMemRefOperands.begin(), oldMemRefOperands.end());

    SmallVector<Value *, 4> remapOutputs;
    remapOutputs.reserve(oldMemRefRank);

    if (indexRemap &&
        indexRemap != builder.getMultiDimIdentityMap(indexRemap.getNumDims())) {
      // Remapped indices.
      for (auto resultExpr : indexRemap.getResults()) {
        auto singleResMap = builder.getAffineMap(
            indexRemap.getNumDims(), indexRemap.getNumSymbols(), resultExpr);
        auto afOp = builder.create<AffineApplyOp>(opInst->getLoc(),
                                                  singleResMap, remapOperands);
        remapOutputs.push_back(afOp);
        affineApplyOps.push_back(afOp);
      }
    } else {
      // No remapping specified.
      remapOutputs.append(remapOperands.begin(), remapOperands.end());
    }

    SmallVector<Value *, 4> newMapOperands;
    newMapOperands.reserve(newMemRefRank);

    // Prepend 'extraIndices' in 'newMapOperands'.
    for (auto *extraIndex : extraIndices) {
      assert(extraIndex->getDefiningOp()->getNumResults() == 1 &&
             "single result op's expected to generate these indices");
      assert((isValidDim(extraIndex) || isValidSymbol(extraIndex)) &&
             "invalid memory op index");
      newMapOperands.push_back(extraIndex);
    }

    // Append 'remapOutputs' to 'newMapOperands'.
    newMapOperands.append(remapOutputs.begin(), remapOutputs.end());

    // Create new fully composed AffineMap for new op to be created.
    assert(newMapOperands.size() == newMemRefRank);
    auto newMap = builder.getMultiDimIdentityMap(newMemRefRank);
    // TODO(b/136262594) Avoid creating/deleting temporary AffineApplyOps here.
    fullyComposeAffineMapAndOperands(&newMap, &newMapOperands);
    newMap = simplifyAffineMap(newMap);
    canonicalizeMapAndOperands(&newMap, &newMapOperands);
    // Remove any affine.apply's that became dead as a result of composition.
    for (auto *value : affineApplyOps)
      if (value->use_empty())
        value->getDefiningOp()->erase();

    // Construct the new operation using this memref.
    OperationState state(opInst->getLoc(), opInst->getName());
    state.setOperandListToResizable(opInst->hasResizableOperandsList());
    state.operands.reserve(opInst->getNumOperands() + extraIndices.size());
    // Insert the non-memref operands.
    state.operands.append(opInst->operand_begin(),
                          opInst->operand_begin() + memRefOperandPos);
    // Insert the new memref value.
    state.operands.push_back(newMemRef);

    // Insert the new memref map operands.
    state.operands.append(newMapOperands.begin(), newMapOperands.end());

    // Insert the remaining operands unmodified.
    state.operands.append(opInst->operand_begin() + memRefOperandPos + 1 +
                              oldMapNumInputs,
                          opInst->operand_end());

    // Result types don't change. Both memref's are of the same elemental type.
    state.types.reserve(opInst->getNumResults());
    for (auto *result : opInst->getResults())
      state.types.push_back(result->getType());

    // Add attribute for 'newMap', other Attributes do not change.
    auto newMapAttr = builder.getAffineMapAttr(newMap);
    for (auto namedAttr : opInst->getAttrs()) {
      if (namedAttr.first == oldMapAttrPair.first) {
        state.attributes.push_back({namedAttr.first, newMapAttr});
      } else {
        state.attributes.push_back(namedAttr);
      }
    }

    // Create the new operation.
    auto *repOp = builder.createOperation(state);
    // Replace old memref's deferencing op's uses.
    unsigned r = 0;
    for (auto *res : opInst->getResults()) {
      res->replaceAllUsesWith(repOp->getResult(r++));
    }
    // Collect and erase at the end since one of these op's could be
    // domInstFilter or postDomInstFilter as well!
    opsToErase.push_back(opInst);
  }

  for (auto *opInst : opsToErase)
    opInst->erase();

  return true;
}

/// Given an operation, inserts one or more single result affine
/// apply operations, results of which are exclusively used by this operation
/// operation. The operands of these newly created affine apply ops are
/// guaranteed to be loop iterators or terminal symbols of a function.
///
/// Before
///
/// affine.for %i = 0 to #map(%N)
///   %idx = affine.apply (d0) -> (d0 mod 2) (%i)
///   "send"(%idx, %A, ...)
///   "compute"(%idx)
///
/// After
///
/// affine.for %i = 0 to #map(%N)
///   %idx = affine.apply (d0) -> (d0 mod 2) (%i)
///   "send"(%idx, %A, ...)
///   %idx_ = affine.apply (d0) -> (d0 mod 2) (%i)
///   "compute"(%idx_)
///
/// This allows applying different transformations on send and compute (for eg.
/// different shifts/delays).
///
/// Returns nullptr either if none of opInst's operands were the result of an
/// affine.apply and thus there was no affine computation slice to create, or if
/// all the affine.apply op's supplying operands to this opInst did not have any
/// uses besides this opInst; otherwise returns the list of affine.apply
/// operations created in output argument `sliceOps`.
void mlir::createAffineComputationSlice(
    Operation *opInst, SmallVectorImpl<AffineApplyOp> *sliceOps) {
  // Collect all operands that are results of affine apply ops.
  SmallVector<Value *, 4> subOperands;
  subOperands.reserve(opInst->getNumOperands());
  for (auto *operand : opInst->getOperands())
    if (isa_and_nonnull<AffineApplyOp>(operand->getDefiningOp()))
      subOperands.push_back(operand);

  // Gather sequence of AffineApplyOps reachable from 'subOperands'.
  SmallVector<Operation *, 4> affineApplyOps;
  getReachableAffineApplyOps(subOperands, affineApplyOps);
  // Skip transforming if there are no affine maps to compose.
  if (affineApplyOps.empty())
    return;

  // Check if all uses of the affine apply op's lie only in this op op, in
  // which case there would be nothing to do.
  bool localized = true;
  for (auto *op : affineApplyOps) {
    for (auto *result : op->getResults()) {
      for (auto *user : result->getUsers()) {
        if (user != opInst) {
          localized = false;
          break;
        }
      }
    }
  }
  if (localized)
    return;

  OpBuilder builder(opInst);
  SmallVector<Value *, 4> composedOpOperands(subOperands);
  auto composedMap = builder.getMultiDimIdentityMap(composedOpOperands.size());
  fullyComposeAffineMapAndOperands(&composedMap, &composedOpOperands);

  // Create an affine.apply for each of the map results.
  sliceOps->reserve(composedMap.getNumResults());
  for (auto resultExpr : composedMap.getResults()) {
    auto singleResMap = builder.getAffineMap(
        composedMap.getNumDims(), composedMap.getNumSymbols(), resultExpr);
    sliceOps->push_back(builder.create<AffineApplyOp>(
        opInst->getLoc(), singleResMap, composedOpOperands));
  }

  // Construct the new operands that include the results from the composed
  // affine apply op above instead of existing ones (subOperands). So, they
  // differ from opInst's operands only for those operands in 'subOperands', for
  // which they will be replaced by the corresponding one from 'sliceOps'.
  SmallVector<Value *, 4> newOperands(opInst->getOperands());
  for (unsigned i = 0, e = newOperands.size(); i < e; i++) {
    // Replace the subOperands from among the new operands.
    unsigned j, f;
    for (j = 0, f = subOperands.size(); j < f; j++) {
      if (newOperands[i] == subOperands[j])
        break;
    }
    if (j < subOperands.size()) {
      newOperands[i] = (*sliceOps)[j];
    }
  }
  for (unsigned idx = 0, e = newOperands.size(); idx < e; idx++) {
    opInst->setOperand(idx, newOperands[idx]);
  }
}
