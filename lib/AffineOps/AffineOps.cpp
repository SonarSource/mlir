//===- AffineOps.cpp - MLIR Affine Operations -----------------------------===//
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

#include "mlir/AffineOps/AffineOps.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/StandardOps/Ops.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/Debug.h"
using namespace mlir;
using llvm::dbgs;

#define DEBUG_TYPE "affine-analysis"

//===----------------------------------------------------------------------===//
// AffineOpsDialect
//===----------------------------------------------------------------------===//

AffineOpsDialect::AffineOpsDialect(MLIRContext *context)
    : Dialect(getDialectNamespace(), context) {
  addOperations<AffineApplyOp, AffineDmaStartOp, AffineDmaWaitOp, AffineForOp,
                AffineIfOp, AffineLoadOp, AffineStoreOp, AffineTerminatorOp>();
}

/// A utility function to check if a given region is attached to a function.
static bool isFunctionRegion(Region *region) {
  return llvm::isa<FuncOp>(region->getContainingOp());
}

/// A utility function to check if a value is defined at the top level of a
/// function. A value defined at the top level is always a valid symbol.
bool mlir::isTopLevelSymbol(Value *value) {
  if (auto *arg = dyn_cast<BlockArgument>(value))
    return isFunctionRegion(arg->getOwner()->getParent());
  return isFunctionRegion(value->getDefiningOp()->getContainingRegion());
}

// Value can be used as a dimension id if it is valid as a symbol, or
// it is an induction variable, or it is a result of affine apply operation
// with dimension id arguments.
bool mlir::isValidDim(Value *value) {
  // The value must be an index type.
  if (!value->getType().isIndex())
    return false;

  if (auto *op = value->getDefiningOp()) {
    // Top level operation or constant operation is ok.
    if (isFunctionRegion(op->getContainingRegion()) || isa<ConstantOp>(op))
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto applyOp = dyn_cast<AffineApplyOp>(op))
      return applyOp.isValidDim();
    // The dim op is okay if its operand memref/tensor is defined at the top
    // level.
    if (auto dimOp = dyn_cast<DimOp>(op))
      return isTopLevelSymbol(dimOp.getOperand());
    return false;
  }
  // This value is a block argument (which also includes 'affine.for' loop IVs).
  return true;
}

// Value can be used as a symbol if it is a constant, or it is defined at
// the top level, or it is a result of affine apply operation with symbol
// arguments.
bool mlir::isValidSymbol(Value *value) {
  // The value must be an index type.
  if (!value->getType().isIndex())
    return false;

  if (auto *op = value->getDefiningOp()) {
    // Top level operation or constant operation is ok.
    if (isFunctionRegion(op->getContainingRegion()) || isa<ConstantOp>(op))
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto applyOp = dyn_cast<AffineApplyOp>(op))
      return applyOp.isValidSymbol();
    // The dim op is okay if its operand memref/tensor is defined at the top
    // level.
    if (auto dimOp = dyn_cast<DimOp>(op))
      return isTopLevelSymbol(dimOp.getOperand());
    return false;
  }
  // Otherwise, check that the value is a top level symbol.
  return isTopLevelSymbol(value);
}

/// Utility function to verify that a set of operands are valid dimension and
/// symbol identifiers. The operands should be layed out such that the dimension
/// operands are before the symbol operands. This function returns failure if
/// there was an invalid operand. An operation is provided to emit any necessary
/// errors.
template <typename OpTy>
static LogicalResult
verifyDimAndSymbolIdentifiers(OpTy &op, Operation::operand_range operands,
                              unsigned numDims) {
  unsigned opIt = 0;
  for (auto *operand : operands) {
    if (opIt++ < numDims) {
      if (!isValidDim(operand))
        return op.emitOpError("operand cannot be used as a dimension id");
    } else if (!isValidSymbol(operand)) {
      return op.emitOpError("operand cannot be used as a symbol");
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AffineApplyOp
//===----------------------------------------------------------------------===//

void AffineApplyOp::build(Builder *builder, OperationState *result,
                          AffineMap map, ArrayRef<Value *> operands) {
  result->addOperands(operands);
  result->types.append(map.getNumResults(), builder->getIndexType());
  result->addAttribute("map", builder->getAffineMapAttr(map));
}

ParseResult AffineApplyOp::parse(OpAsmParser *parser, OperationState *result) {
  auto &builder = parser->getBuilder();
  auto affineIntTy = builder.getIndexType();

  AffineMapAttr mapAttr;
  unsigned numDims;
  if (parser->parseAttribute(mapAttr, "map", result->attributes) ||
      parseDimAndSymbolList(parser, result->operands, numDims) ||
      parser->parseOptionalAttributeDict(result->attributes))
    return failure();
  auto map = mapAttr.getValue();

  if (map.getNumDims() != numDims ||
      numDims + map.getNumSymbols() != result->operands.size()) {
    return parser->emitError(parser->getNameLoc(),
                             "dimension or symbol index mismatch");
  }

  result->types.append(map.getNumResults(), affineIntTy);
  return success();
}

void AffineApplyOp::print(OpAsmPrinter *p) {
  *p << "affine.apply " << getAttr("map");
  printDimAndSymbolList(operand_begin(), operand_end(),
                        getAffineMap().getNumDims(), p);
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{"map"});
}

LogicalResult AffineApplyOp::verify() {
  // Check that affine map attribute was specified.
  auto affineMapAttr = getAttrOfType<AffineMapAttr>("map");
  if (!affineMapAttr)
    return emitOpError("requires an affine map");

  // Check input and output dimensions match.
  auto map = affineMapAttr.getValue();

  // Verify that operand count matches affine map dimension and symbol count.
  if (getNumOperands() != map.getNumDims() + map.getNumSymbols())
    return emitOpError(
        "operand count and affine map dimension and symbol count must match");

  // Verify that all operands are of `index` type.
  for (Type t : getOperandTypes()) {
    if (!t.isIndex())
      return emitOpError("operands must be of type 'index'");
  }

  if (!getResult()->getType().isIndex())
    return emitOpError("result must be of type 'index'");

  // Verify that the operands are valid dimension and symbol identifiers.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getOperands(),
                                           map.getNumDims())))
    return failure();

  // Verify that the map only produces one result.
  if (map.getNumResults() != 1)
    return emitOpError("mapping must produce one value");

  return success();
}

// The result of the affine apply operation can be used as a dimension id if it
// is a CFG value or if it is an Value, and all the operands are valid
// dimension ids.
bool AffineApplyOp::isValidDim() {
  return llvm::all_of(getOperands(),
                      [](Value *op) { return mlir::isValidDim(op); });
}

// The result of the affine apply operation can be used as a symbol if it is
// a CFG value or if it is an Value, and all the operands are symbols.
bool AffineApplyOp::isValidSymbol() {
  return llvm::all_of(getOperands(),
                      [](Value *op) { return mlir::isValidSymbol(op); });
}

OpFoldResult AffineApplyOp::fold(ArrayRef<Attribute> operands) {
  auto map = getAffineMap();

  // Fold dims and symbols to existing values.
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return getOperand(dim.getPosition());
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return getOperand(map.getNumDims() + sym.getPosition());

  // Otherwise, default to folding the map.
  SmallVector<Attribute, 1> result;
  if (failed(map.constantFold(operands, result)))
    return {};
  return result[0];
}

namespace {
/// An `AffineApplyNormalizer` is a helper class that is not visible to the user
/// and supports renumbering operands of AffineApplyOp. This acts as a
/// reindexing map of Value* to positional dims or symbols and allows
/// simplifications such as:
///
/// ```mlir
///    %1 = affine.apply (d0, d1) -> (d0 - d1) (%0, %0)
/// ```
///
/// into:
///
/// ```mlir
///    %1 = affine.apply () -> (0)
/// ```
struct AffineApplyNormalizer {
  AffineApplyNormalizer(AffineMap map, ArrayRef<Value *> operands);

  /// Returns the AffineMap resulting from normalization.
  AffineMap getAffineMap() { return affineMap; }

  SmallVector<Value *, 8> getOperands() {
    SmallVector<Value *, 8> res(reorderedDims);
    res.append(concatenatedSymbols.begin(), concatenatedSymbols.end());
    return res;
  }

private:
  /// Helper function to insert `v` into the coordinate system of the current
  /// AffineApplyNormalizer. Returns the AffineDimExpr with the corresponding
  /// renumbered position.
  AffineDimExpr renumberOneDim(Value *v);

  /// Given an `other` normalizer, this rewrites `other.affineMap` in the
  /// coordinate system of the current AffineApplyNormalizer.
  /// Returns the rewritten AffineMap and updates the dims and symbols of
  /// `this`.
  AffineMap renumber(const AffineApplyNormalizer &other);

  /// Maps of Value* to position in `affineMap`.
  DenseMap<Value *, unsigned> dimValueToPosition;

  /// Ordered dims and symbols matching positional dims and symbols in
  /// `affineMap`.
  SmallVector<Value *, 8> reorderedDims;
  SmallVector<Value *, 8> concatenatedSymbols;

  AffineMap affineMap;

  /// Used with RAII to control the depth at which AffineApply are composed
  /// recursively. Only accepts depth 1 for now to allow a behavior where a
  /// newly composed AffineApplyOp does not increase the length of the chain of
  /// AffineApplyOps. Full composition is implemented iteratively on top of
  /// this behavior.
  static unsigned &affineApplyDepth() {
    static thread_local unsigned depth = 0;
    return depth;
  }
  static constexpr unsigned kMaxAffineApplyDepth = 1;

  AffineApplyNormalizer() { affineApplyDepth()++; }

public:
  ~AffineApplyNormalizer() { affineApplyDepth()--; }
};
} // end anonymous namespace.

AffineDimExpr AffineApplyNormalizer::renumberOneDim(Value *v) {
  DenseMap<Value *, unsigned>::iterator iterPos;
  bool inserted = false;
  std::tie(iterPos, inserted) =
      dimValueToPosition.insert(std::make_pair(v, dimValueToPosition.size()));
  if (inserted) {
    reorderedDims.push_back(v);
  }
  return getAffineDimExpr(iterPos->second, v->getContext())
      .cast<AffineDimExpr>();
}

AffineMap AffineApplyNormalizer::renumber(const AffineApplyNormalizer &other) {
  SmallVector<AffineExpr, 8> dimRemapping;
  for (auto *v : other.reorderedDims) {
    auto kvp = other.dimValueToPosition.find(v);
    if (dimRemapping.size() <= kvp->second)
      dimRemapping.resize(kvp->second + 1);
    dimRemapping[kvp->second] = renumberOneDim(kvp->first);
  }
  unsigned numSymbols = concatenatedSymbols.size();
  unsigned numOtherSymbols = other.concatenatedSymbols.size();
  SmallVector<AffineExpr, 8> symRemapping(numOtherSymbols);
  for (unsigned idx = 0; idx < numOtherSymbols; ++idx) {
    symRemapping[idx] =
        getAffineSymbolExpr(idx + numSymbols, other.affineMap.getContext());
  }
  concatenatedSymbols.insert(concatenatedSymbols.end(),
                             other.concatenatedSymbols.begin(),
                             other.concatenatedSymbols.end());
  auto map = other.affineMap;
  return map.replaceDimsAndSymbols(dimRemapping, symRemapping,
                                   dimRemapping.size(), symRemapping.size());
}

// Gather the positions of the operands that are produced by an AffineApplyOp.
static llvm::SetVector<unsigned>
indicesFromAffineApplyOp(ArrayRef<Value *> operands) {
  llvm::SetVector<unsigned> res;
  for (auto en : llvm::enumerate(operands))
    if (isa_and_nonnull<AffineApplyOp>(en.value()->getDefiningOp()))
      res.insert(en.index());
  return res;
}

// Support the special case of a symbol coming from an AffineApplyOp that needs
// to be composed into the current AffineApplyOp.
// This case is handled by rewriting all such symbols into dims for the purpose
// of allowing mathematical AffineMap composition.
// Returns an AffineMap where symbols that come from an AffineApplyOp have been
// rewritten as dims and are ordered after the original dims.
// TODO(andydavis,ntv): This promotion makes AffineMap lose track of which
// symbols are represented as dims. This loss is static but can still be
// recovered dynamically (with `isValidSymbol`). Still this is annoying for the
// semi-affine map case. A dynamic canonicalization of all dims that are valid
// symbols (a.k.a `canonicalizePromotedSymbols`) into symbols helps and even
// results in better simplifications and foldings. But we should evaluate
// whether this behavior is what we really want after using more.
static AffineMap promoteComposedSymbolsAsDims(AffineMap map,
                                              ArrayRef<Value *> symbols) {
  if (symbols.empty()) {
    return map;
  }

  // Sanity check on symbols.
  for (auto *sym : symbols) {
    assert(isValidSymbol(sym) && "Expected only valid symbols");
    (void)sym;
  }

  // Extract the symbol positions that come from an AffineApplyOp and
  // needs to be rewritten as dims.
  auto symPositions = indicesFromAffineApplyOp(symbols);
  if (symPositions.empty()) {
    return map;
  }

  // Create the new map by replacing each symbol at pos by the next new dim.
  unsigned numDims = map.getNumDims();
  unsigned numSymbols = map.getNumSymbols();
  unsigned numNewDims = 0;
  unsigned numNewSymbols = 0;
  SmallVector<AffineExpr, 8> symReplacements(numSymbols);
  for (unsigned i = 0; i < numSymbols; ++i) {
    symReplacements[i] =
        symPositions.count(i) > 0
            ? getAffineDimExpr(numDims + numNewDims++, map.getContext())
            : getAffineSymbolExpr(numNewSymbols++, map.getContext());
  }
  assert(numSymbols >= numNewDims);
  AffineMap newMap = map.replaceDimsAndSymbols(
      {}, symReplacements, numDims + numNewDims, numNewSymbols);

  return newMap;
}

/// The AffineNormalizer composes AffineApplyOp recursively. Its purpose is to
/// keep a correspondence between the mathematical `map` and the `operands` of
/// a given AffineApplyOp. This correspondence is maintained by iterating over
/// the operands and forming an `auxiliaryMap` that can be composed
/// mathematically with `map`. To keep this correspondence in cases where
/// symbols are produced by affine.apply operations, we perform a local rewrite
/// of symbols as dims.
///
/// Rationale for locally rewriting symbols as dims:
/// ================================================
/// The mathematical composition of AffineMap must always concatenate symbols
/// because it does not have enough information to do otherwise. For example,
/// composing `(d0)[s0] -> (d0 + s0)` with itself must produce
/// `(d0)[s0, s1] -> (d0 + s0 + s1)`.
///
/// The result is only equivalent to `(d0)[s0] -> (d0 + 2 * s0)` when
/// applied to the same mlir::Value* for both s0 and s1.
/// As a consequence mathematical composition of AffineMap always concatenates
/// symbols.
///
/// When AffineMaps are used in AffineApplyOp however, they may specify
/// composition via symbols, which is ambiguous mathematically. This corner case
/// is handled by locally rewriting such symbols that come from AffineApplyOp
/// into dims and composing through dims.
/// TODO(andydavis, ntv): Composition via symbols comes at a significant code
/// complexity. Alternatively we should investigate whether we want to
/// explicitly disallow symbols coming from affine.apply and instead force the
/// user to compose symbols beforehand. The annoyances may be small (i.e. 1 or 2
/// extra API calls for such uses, which haven't popped up until now) and the
/// benefit potentially big: simpler and more maintainable code for a
/// non-trivial, recursive, procedure.
AffineApplyNormalizer::AffineApplyNormalizer(AffineMap map,
                                             ArrayRef<Value *> operands)
    : AffineApplyNormalizer() {
  static_assert(kMaxAffineApplyDepth > 0, "kMaxAffineApplyDepth must be > 0");
  assert(map.getNumInputs() == operands.size() &&
         "number of operands does not match the number of map inputs");

  LLVM_DEBUG(map.print(dbgs() << "\nInput map: "));

  // Promote symbols that come from an AffineApplyOp to dims by rewriting the
  // map to always refer to:
  //   (dims, symbols coming from AffineApplyOp, other symbols).
  // The order of operands can remain unchanged.
  // This is a simplification that relies on 2 ordering properties:
  //   1. rewritten symbols always appear after the original dims in the map;
  //   2. operands are traversed in order and either dispatched to:
  //      a. auxiliaryExprs (dims and symbols rewritten as dims);
  //      b. concatenatedSymbols (all other symbols)
  // This allows operand order to remain unchanged.
  unsigned numDimsBeforeRewrite = map.getNumDims();
  map = promoteComposedSymbolsAsDims(map,
                                     operands.take_back(map.getNumSymbols()));

  LLVM_DEBUG(map.print(dbgs() << "\nRewritten map: "));

  SmallVector<AffineExpr, 8> auxiliaryExprs;
  bool furtherCompose = (affineApplyDepth() <= kMaxAffineApplyDepth);
  // We fully spell out the 2 cases below. In this particular instance a little
  // code duplication greatly improves readability.
  // Note that the first branch would disappear if we only supported full
  // composition (i.e. infinite kMaxAffineApplyDepth).
  if (!furtherCompose) {
    // 1. Only dispatch dims or symbols.
    for (auto en : llvm::enumerate(operands)) {
      auto *t = en.value();
      assert(t->getType().isIndex());
      bool isDim = (en.index() < map.getNumDims());
      if (isDim) {
        // a. The mathematical composition of AffineMap composes dims.
        auxiliaryExprs.push_back(renumberOneDim(t));
      } else {
        // b. The mathematical composition of AffineMap concatenates symbols.
        //    We do the same for symbol operands.
        concatenatedSymbols.push_back(t);
      }
    }
  } else {
    assert(numDimsBeforeRewrite <= operands.size());
    // 2. Compose AffineApplyOps and dispatch dims or symbols.
    for (unsigned i = 0, e = operands.size(); i < e; ++i) {
      auto *t = operands[i];
      auto affineApply = dyn_cast_or_null<AffineApplyOp>(t->getDefiningOp());
      if (affineApply) {
        // a. Compose affine.apply operations.
        LLVM_DEBUG(affineApply.getOperation()->print(
            dbgs() << "\nCompose AffineApplyOp recursively: "));
        AffineMap affineApplyMap = affineApply.getAffineMap();
        SmallVector<Value *, 8> affineApplyOperands(
            affineApply.getOperands().begin(), affineApply.getOperands().end());
        AffineApplyNormalizer normalizer(affineApplyMap, affineApplyOperands);

        LLVM_DEBUG(normalizer.affineMap.print(
            dbgs() << "\nRenumber into current normalizer: "));

        auto renumberedMap = renumber(normalizer);

        LLVM_DEBUG(
            renumberedMap.print(dbgs() << "\nRecursive composition yields: "));

        auxiliaryExprs.push_back(renumberedMap.getResult(0));
      } else {
        if (i < numDimsBeforeRewrite) {
          // b. The mathematical composition of AffineMap composes dims.
          auxiliaryExprs.push_back(renumberOneDim(t));
        } else {
          // c. The mathematical composition of AffineMap concatenates symbols.
          //    We do the same for symbol operands.
          concatenatedSymbols.push_back(t);
        }
      }
    }
  }

  // Early exit if `map` is already composed.
  if (auxiliaryExprs.empty()) {
    affineMap = map;
    return;
  }

  assert(concatenatedSymbols.size() >= map.getNumSymbols() &&
         "Unexpected number of concatenated symbols");
  auto numDims = dimValueToPosition.size();
  auto numSymbols = concatenatedSymbols.size() - map.getNumSymbols();
  auto auxiliaryMap = AffineMap::get(numDims, numSymbols, auxiliaryExprs);

  LLVM_DEBUG(map.print(dbgs() << "\nCompose map: "));
  LLVM_DEBUG(auxiliaryMap.print(dbgs() << "\nWith map: "));
  LLVM_DEBUG(map.compose(auxiliaryMap).print(dbgs() << "\nResult: "));

  // TODO(andydavis,ntv): Disabling simplification results in major speed gains.
  // Another option is to cache the results as it is expected a lot of redundant
  // work is performed in practice.
  affineMap = simplifyAffineMap(map.compose(auxiliaryMap));

  LLVM_DEBUG(affineMap.print(dbgs() << "\nSimplified result: "));
  LLVM_DEBUG(dbgs() << "\n");
}

/// Implements `map` and `operands` composition and simplification to support
/// `makeComposedAffineApply`. This can be called to achieve the same effects
/// on `map` and `operands` without creating an AffineApplyOp that needs to be
/// immediately deleted.
static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value *> *operands) {
  AffineApplyNormalizer normalizer(*map, *operands);
  auto normalizedMap = normalizer.getAffineMap();
  auto normalizedOperands = normalizer.getOperands();
  canonicalizeMapAndOperands(&normalizedMap, &normalizedOperands);
  *map = normalizedMap;
  *operands = normalizedOperands;
  assert(*map);
}

void mlir::fullyComposeAffineMapAndOperands(
    AffineMap *map, SmallVectorImpl<Value *> *operands) {
  while (llvm::any_of(*operands, [](Value *v) {
    return isa_and_nonnull<AffineApplyOp>(v->getDefiningOp());
  })) {
    composeAffineMapAndOperands(map, operands);
  }
}

AffineApplyOp mlir::makeComposedAffineApply(OpBuilder &b, Location loc,
                                            AffineMap map,
                                            ArrayRef<Value *> operands) {
  AffineMap normalizedMap = map;
  SmallVector<Value *, 8> normalizedOperands(operands.begin(), operands.end());
  composeAffineMapAndOperands(&normalizedMap, &normalizedOperands);
  assert(normalizedMap);
  return b.create<AffineApplyOp>(loc, normalizedMap, normalizedOperands);
}

// A symbol may appear as a dim in affine.apply operations. This function
// canonicalizes dims that are valid symbols into actual symbols.
static void
canonicalizePromotedSymbols(AffineMap *map,
                            llvm::SmallVectorImpl<Value *> *operands) {
  if (!map || operands->empty())
    return;

  assert(map->getNumInputs() == operands->size() &&
         "map inputs must match number of operands");

  auto *context = map->getContext();
  SmallVector<Value *, 8> resultOperands;
  resultOperands.reserve(operands->size());
  SmallVector<Value *, 8> remappedSymbols;
  remappedSymbols.reserve(operands->size());
  unsigned nextDim = 0;
  unsigned nextSym = 0;
  unsigned oldNumSyms = map->getNumSymbols();
  SmallVector<AffineExpr, 8> dimRemapping(map->getNumDims());
  for (unsigned i = 0, e = map->getNumInputs(); i != e; ++i) {
    if (i < map->getNumDims()) {
      if (isValidSymbol((*operands)[i])) {
        // This is a valid symbols that appears as a dim, canonicalize it.
        dimRemapping[i] = getAffineSymbolExpr(oldNumSyms + nextSym++, context);
        remappedSymbols.push_back((*operands)[i]);
      } else {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
      }
    } else {
      resultOperands.push_back((*operands)[i]);
    }
  }

  resultOperands.append(remappedSymbols.begin(), remappedSymbols.end());
  *operands = resultOperands;
  *map = map->replaceDimsAndSymbols(dimRemapping, {}, nextDim,
                                    oldNumSyms + nextSym);

  assert(map->getNumInputs() == operands->size() &&
         "map inputs must match number of operands");
}

void mlir::canonicalizeMapAndOperands(
    AffineMap *map, llvm::SmallVectorImpl<Value *> *operands) {
  if (!map || operands->empty())
    return;

  assert(map->getNumInputs() == operands->size() &&
         "map inputs must match number of operands");

  canonicalizePromotedSymbols(map, operands);

  // Check to see what dims are used.
  llvm::SmallBitVector usedDims(map->getNumDims());
  llvm::SmallBitVector usedSyms(map->getNumSymbols());
  map->walkExprs([&](AffineExpr expr) {
    if (auto dimExpr = expr.dyn_cast<AffineDimExpr>())
      usedDims[dimExpr.getPosition()] = true;
    else if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>())
      usedSyms[symExpr.getPosition()] = true;
  });

  auto *context = map->getContext();

  SmallVector<Value *, 8> resultOperands;
  resultOperands.reserve(operands->size());

  llvm::SmallDenseMap<Value *, AffineExpr, 8> seenDims;
  SmallVector<AffineExpr, 8> dimRemapping(map->getNumDims());
  unsigned nextDim = 0;
  for (unsigned i = 0, e = map->getNumDims(); i != e; ++i) {
    if (usedDims[i]) {
      auto it = seenDims.find((*operands)[i]);
      if (it == seenDims.end()) {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
        seenDims.insert(std::make_pair((*operands)[i], dimRemapping[i]));
      } else {
        dimRemapping[i] = it->second;
      }
    }
  }
  llvm::SmallDenseMap<Value *, AffineExpr, 8> seenSymbols;
  SmallVector<AffineExpr, 8> symRemapping(map->getNumSymbols());
  unsigned nextSym = 0;
  for (unsigned i = 0, e = map->getNumSymbols(); i != e; ++i) {
    if (usedSyms[i]) {
      auto it = seenSymbols.find((*operands)[i + map->getNumDims()]);
      if (it == seenSymbols.end()) {
        symRemapping[i] = getAffineSymbolExpr(nextSym++, context);
        resultOperands.push_back((*operands)[i + map->getNumDims()]);
        seenSymbols.insert(std::make_pair((*operands)[i + map->getNumDims()],
                                          symRemapping[i]));
      } else {
        symRemapping[i] = it->second;
      }
    }
  }
  *map =
      map->replaceDimsAndSymbols(dimRemapping, symRemapping, nextDim, nextSym);
  *operands = resultOperands;
}

namespace {
/// Simplify AffineApply operations.
///
struct SimplifyAffineApply : public OpRewritePattern<AffineApplyOp> {
  using OpRewritePattern<AffineApplyOp>::OpRewritePattern;

  PatternMatchResult matchAndRewrite(AffineApplyOp apply,
                                     PatternRewriter &rewriter) const override {
    auto map = apply.getAffineMap();

    AffineMap oldMap = map;
    SmallVector<Value *, 8> resultOperands(apply.getOperands());
    composeAffineMapAndOperands(&map, &resultOperands);
    if (map == oldMap)
      return matchFailure();

    rewriter.replaceOpWithNewOp<AffineApplyOp>(apply, map, resultOperands);
    return matchSuccess();
  }
};
} // end anonymous namespace.

void AffineApplyOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyAffineApply>(context));
}

//===----------------------------------------------------------------------===//
// Common canonicalization pattern support logic
//===----------------------------------------------------------------------===//

namespace {
/// This is a common class used for patterns of the form
/// "someop(memrefcast) -> someop".  It folds the source of any memref_cast
/// into the root operation directly.
struct MemRefCastFolder : public RewritePattern {
  /// The rootOpName is the name of the root operation to match against.
  MemRefCastFolder(StringRef rootOpName, MLIRContext *context)
      : RewritePattern(rootOpName, 1, context) {}

  PatternMatchResult match(Operation *op) const override {
    for (auto *operand : op->getOperands())
      if (matchPattern(operand, m_Op<MemRefCastOp>()))
        return matchSuccess();

    return matchFailure();
  }

  void rewrite(Operation *op, PatternRewriter &rewriter) const override {
    for (unsigned i = 0, e = op->getNumOperands(); i != e; ++i)
      if (auto *memref = op->getOperand(i)->getDefiningOp())
        if (auto cast = dyn_cast<MemRefCastOp>(memref))
          op->setOperand(i, cast.getOperand());
    rewriter.updatedRootInPlace(op);
  }
};

} // end anonymous namespace.

//===----------------------------------------------------------------------===//
// AffineDmaStartOp
//===----------------------------------------------------------------------===//

// TODO(b/133776335) Check that map operands are loop IVs or symbols.
void AffineDmaStartOp::build(Builder *builder, OperationState *result,
                             Value *srcMemRef, AffineMap srcMap,
                             ArrayRef<Value *> srcIndices, Value *destMemRef,
                             AffineMap dstMap, ArrayRef<Value *> destIndices,
                             Value *tagMemRef, AffineMap tagMap,
                             ArrayRef<Value *> tagIndices, Value *numElements,
                             Value *stride, Value *elementsPerStride) {
  result->addOperands(srcMemRef);
  result->addAttribute(getSrcMapAttrName(), builder->getAffineMapAttr(srcMap));
  result->addOperands(srcIndices);
  result->addOperands(destMemRef);
  result->addAttribute(getDstMapAttrName(), builder->getAffineMapAttr(dstMap));
  result->addOperands(destIndices);
  result->addOperands(tagMemRef);
  result->addAttribute(getTagMapAttrName(), builder->getAffineMapAttr(tagMap));
  result->addOperands(tagIndices);
  result->addOperands(numElements);
  if (stride) {
    result->addOperands({stride, elementsPerStride});
  }
}

void AffineDmaStartOp::print(OpAsmPrinter *p) {
  *p << "affine.dma_start " << *getSrcMemRef() << '[';
  SmallVector<Value *, 8> operands(getSrcIndices());
  p->printAffineMapOfSSAIds(getSrcMapAttr(), operands);
  *p << "], " << *getDstMemRef() << '[';
  operands.assign(getDstIndices().begin(), getDstIndices().end());
  p->printAffineMapOfSSAIds(getDstMapAttr(), operands);
  *p << "], " << *getTagMemRef() << '[';
  operands.assign(getTagIndices().begin(), getTagIndices().end());
  p->printAffineMapOfSSAIds(getTagMapAttr(), operands);
  *p << "], " << *getNumElements();
  if (isStrided()) {
    *p << ", " << *getStride();
    *p << ", " << *getNumElementsPerStride();
  }
  *p << " : " << getSrcMemRefType() << ", " << getDstMemRefType() << ", "
     << getTagMemRefType();
}

// Parse AffineDmaStartOp.
// Ex:
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%index], %size,
//     %stride, %num_elt_per_stride
//       : memref<3076 x f32, 0>, memref<1024 x f32, 2>, memref<1 x i32>
//
ParseResult AffineDmaStartOp::parse(OpAsmParser *parser,
                                    OperationState *result) {
  OpAsmParser::OperandType srcMemRefInfo;
  AffineMapAttr srcMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> srcMapOperands;
  OpAsmParser::OperandType dstMemRefInfo;
  AffineMapAttr dstMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> dstMapOperands;
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> tagMapOperands;
  OpAsmParser::OperandType numElementsInfo;
  SmallVector<OpAsmParser::OperandType, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser->getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) dst memref followed by its affine maps operands (in square brackets).
  // *) src memref followed by its affine map operands (in square brackets).
  // *) tag memref followed by its affine map operands (in square brackets).
  // *) number of elements transferred by DMA operation.
  if (parser->parseOperand(srcMemRefInfo) ||
      parser->parseAffineMapOfSSAIds(srcMapOperands, srcMapAttr,
                                     getSrcMapAttrName(), result->attributes) ||
      parser->parseComma() || parser->parseOperand(dstMemRefInfo) ||
      parser->parseAffineMapOfSSAIds(dstMapOperands, dstMapAttr,
                                     getDstMapAttrName(), result->attributes) ||
      parser->parseComma() || parser->parseOperand(tagMemRefInfo) ||
      parser->parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                     getTagMapAttrName(), result->attributes) ||
      parser->parseComma() || parser->parseOperand(numElementsInfo))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser->parseTrailingOperandList(strideInfo)) {
    return failure();
  }
  if (!strideInfo.empty() && strideInfo.size() != 2) {
    return parser->emitError(parser->getNameLoc(),
                             "expected two stride related operands");
  }
  bool isStrided = strideInfo.size() == 2;

  if (parser->parseColonTypeList(types))
    return failure();

  if (types.size() != 3)
    return parser->emitError(parser->getNameLoc(), "expected three types");

  if (parser->resolveOperand(srcMemRefInfo, types[0], result->operands) ||
      parser->resolveOperands(srcMapOperands, indexType, result->operands) ||
      parser->resolveOperand(dstMemRefInfo, types[1], result->operands) ||
      parser->resolveOperands(dstMapOperands, indexType, result->operands) ||
      parser->resolveOperand(tagMemRefInfo, types[2], result->operands) ||
      parser->resolveOperands(tagMapOperands, indexType, result->operands) ||
      parser->resolveOperand(numElementsInfo, indexType, result->operands))
    return failure();

  if (isStrided) {
    if (parser->resolveOperands(strideInfo, indexType, result->operands))
      return failure();
  }

  // Check that src/dst/tag operand counts match their map.numInputs.
  if (srcMapOperands.size() != srcMapAttr.getValue().getNumInputs() ||
      dstMapOperands.size() != dstMapAttr.getValue().getNumInputs() ||
      tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser->emitError(parser->getNameLoc(),
                             "memref operand count not equal to map.numInputs");
  return success();
}

LogicalResult AffineDmaStartOp::verify() {
  if (!getOperand(getSrcMemRefOperandIndex())->getType().isa<MemRefType>())
    return emitOpError("expected DMA source to be of memref type");
  if (!getOperand(getDstMemRefOperandIndex())->getType().isa<MemRefType>())
    return emitOpError("expected DMA destination to be of memref type");
  if (!getOperand(getTagMemRefOperandIndex())->getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");

  // DMAs from different memory spaces supported.
  if (getSrcMemorySpace() == getDstMemorySpace()) {
    return emitOpError("DMA should be between different memory spaces");
  }
  unsigned numInputsAllMaps = getSrcMap().getNumInputs() +
                              getDstMap().getNumInputs() +
                              getTagMap().getNumInputs();
  if (getNumOperands() != numInputsAllMaps + 3 + 1 &&
      getNumOperands() != numInputsAllMaps + 3 + 1 + 2) {
    return emitOpError("incorrect number of operands");
  }
  return success();
}

void AffineDmaStartOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  /// dma_start(memrefcast) -> dma_start
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// AffineDmaWaitOp
//===----------------------------------------------------------------------===//

// TODO(b/133776335) Check that map operands are loop IVs or symbols.
void AffineDmaWaitOp::build(Builder *builder, OperationState *result,
                            Value *tagMemRef, AffineMap tagMap,
                            ArrayRef<Value *> tagIndices, Value *numElements) {
  result->addOperands(tagMemRef);
  result->addAttribute(getTagMapAttrName(), builder->getAffineMapAttr(tagMap));
  result->addOperands(tagIndices);
  result->addOperands(numElements);
}

void AffineDmaWaitOp::print(OpAsmPrinter *p) {
  *p << "affine.dma_wait " << *getTagMemRef() << '[';
  SmallVector<Value *, 2> operands(getTagIndices());
  p->printAffineMapOfSSAIds(getTagMapAttr(), operands);
  *p << "], ";
  p->printOperand(getNumElements());
  *p << " : " << getTagMemRef()->getType();
}

// Parse AffineDmaWaitOp.
// Eg:
//   affine.dma_wait %tag[%index], %num_elements
//     : memref<1 x i32, (d0) -> (d0), 4>
//
ParseResult AffineDmaWaitOp::parse(OpAsmParser *parser,
                                   OperationState *result) {
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 2> tagMapOperands;
  Type type;
  auto indexType = parser->getBuilder().getIndexType();
  OpAsmParser::OperandType numElementsInfo;

  // Parse tag memref, its map operands, and dma size.
  if (parser->parseOperand(tagMemRefInfo) ||
      parser->parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                     getTagMapAttrName(), result->attributes) ||
      parser->parseComma() || parser->parseOperand(numElementsInfo) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(tagMemRefInfo, type, result->operands) ||
      parser->resolveOperands(tagMapOperands, indexType, result->operands) ||
      parser->resolveOperand(numElementsInfo, indexType, result->operands))
    return failure();

  if (!type.isa<MemRefType>())
    return parser->emitError(parser->getNameLoc(),
                             "expected tag to be of memref type");

  if (tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser->emitError(parser->getNameLoc(),
                             "tag memref operand count != to map.numInputs");
  return success();
}

LogicalResult AffineDmaWaitOp::verify() {
  if (!getOperand(0)->getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");
  return success();
}

void AffineDmaWaitOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  /// dma_wait(memrefcast) -> dma_wait
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// AffineForOp
//===----------------------------------------------------------------------===//

// Check that if a "block" has a terminator, it is an `AffineTerminatorOp`.
static LogicalResult checkHasAffineTerminator(OpState &op, Block &block) {
  if (block.empty() || isa<AffineTerminatorOp>(block.back()))
    return success();

  return op.emitOpError("expects regions to end with '" +
                        AffineTerminatorOp::getOperationName() + "'")
             .attachNote()
         << "in custom textual format, the absence of terminator implies '"
         << AffineTerminatorOp::getOperationName() << "'";
}

// Insert `affine.terminator` at the end of the region's only block if it does
// not have a terminator already.  If the region is empty, insert a new block
// first.
static void ensureAffineTerminator(Region &region, Builder &builder,
                                   Location loc) {
  impl::ensureRegionTerminator<AffineTerminatorOp>(region, builder, loc);
}

void AffineForOp::build(Builder *builder, OperationState *result,
                        ArrayRef<Value *> lbOperands, AffineMap lbMap,
                        ArrayRef<Value *> ubOperands, AffineMap ubMap,
                        int64_t step) {
  assert(((!lbMap && lbOperands.empty()) ||
          lbOperands.size() == lbMap.getNumInputs()) &&
         "lower bound operand count does not match the affine map");
  assert(((!ubMap && ubOperands.empty()) ||
          ubOperands.size() == ubMap.getNumInputs()) &&
         "upper bound operand count does not match the affine map");
  assert(step > 0 && "step has to be a positive integer constant");

  // Add an attribute for the step.
  result->addAttribute(getStepAttrName(),
                       builder->getIntegerAttr(builder->getIndexType(), step));

  // Add the lower bound.
  result->addAttribute(getLowerBoundAttrName(),
                       builder->getAffineMapAttr(lbMap));
  result->addOperands(lbOperands);

  // Add the upper bound.
  result->addAttribute(getUpperBoundAttrName(),
                       builder->getAffineMapAttr(ubMap));
  result->addOperands(ubOperands);

  // Create a region and a block for the body.  The argument of the region is
  // the loop induction variable.
  Region *bodyRegion = result->addRegion();
  Block *body = new Block();
  body->addArgument(IndexType::get(builder->getContext()));
  bodyRegion->push_back(body);
  ensureAffineTerminator(*bodyRegion, *builder, result->location);

  // Set the operands list as resizable so that we can freely modify the bounds.
  result->setOperandListToResizable();
}

void AffineForOp::build(Builder *builder, OperationState *result, int64_t lb,
                        int64_t ub, int64_t step) {
  auto lbMap = AffineMap::getConstantMap(lb, builder->getContext());
  auto ubMap = AffineMap::getConstantMap(ub, builder->getContext());
  return build(builder, result, {}, lbMap, {}, ubMap, step);
}

LogicalResult AffineForOp::verify() {
  auto &bodyRegion = getOperation()->getRegion(0);

  // The body region must contain a single basic block.
  if (bodyRegion.empty() || std::next(bodyRegion.begin()) != bodyRegion.end())
    return emitOpError("expected body region to have a single block");

  // Check that the body defines as single block argument for the induction
  // variable.
  auto *body = getBody();
  if (body->getNumArguments() != 1 ||
      !body->getArgument(0)->getType().isIndex())
    return emitOpError("expected body to have a single index argument for the "
                       "induction variable");

  if (failed(checkHasAffineTerminator(*this, *body)))
    return failure();

  // Verify that there are enough operands for the bounds.
  AffineMap lowerBoundMap = getLowerBoundMap(),
            upperBoundMap = getUpperBoundMap();
  if (getNumOperands() !=
      (lowerBoundMap.getNumInputs() + upperBoundMap.getNumInputs()))
    return emitOpError(
        "operand count must match with affine map dimension and symbol count");

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bound.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getLowerBoundOperands(),
                                           getLowerBoundMap().getNumDims())))
    return failure();
  /// Upper bound.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getUpperBoundOperands(),
                                           getUpperBoundMap().getNumDims())))
    return failure();
  return success();
}

/// Parse a for operation loop bounds.
static ParseResult parseBound(bool isLower, OperationState *result,
                              OpAsmParser *p) {
  // 'min' / 'max' prefixes are generally syntactic sugar, but are required if
  // the map has multiple results.
  bool failedToParsedMinMax =
      failed(p->parseOptionalKeyword(isLower ? "max" : "min"));

  auto &builder = p->getBuilder();
  auto boundAttrName = isLower ? AffineForOp::getLowerBoundAttrName()
                               : AffineForOp::getUpperBoundAttrName();

  // Parse ssa-id as identity map.
  SmallVector<OpAsmParser::OperandType, 1> boundOpInfos;
  if (p->parseOperandList(boundOpInfos))
    return failure();

  if (!boundOpInfos.empty()) {
    // Check that only one operand was parsed.
    if (boundOpInfos.size() > 1)
      return p->emitError(p->getNameLoc(),
                          "expected only one loop bound operand");

    // TODO: improve error message when SSA value is not an affine integer.
    // Currently it is 'use of value ... expects different type than prior uses'
    if (p->resolveOperand(boundOpInfos.front(), builder.getIndexType(),
                          result->operands))
      return failure();

    // Create an identity map using symbol id. This representation is optimized
    // for storage. Analysis passes may expand it into a multi-dimensional map
    // if desired.
    AffineMap map = builder.getSymbolIdentityMap();
    result->addAttribute(boundAttrName, builder.getAffineMapAttr(map));
    return success();
  }

  // Get the attribute location.
  llvm::SMLoc attrLoc = p->getCurrentLocation();

  Attribute boundAttr;
  if (p->parseAttribute(boundAttr, builder.getIndexType(), boundAttrName,
                        result->attributes))
    return failure();

  // Parse full form - affine map followed by dim and symbol list.
  if (auto affineMapAttr = boundAttr.dyn_cast<AffineMapAttr>()) {
    unsigned currentNumOperands = result->operands.size();
    unsigned numDims;
    if (parseDimAndSymbolList(p, result->operands, numDims))
      return failure();

    auto map = affineMapAttr.getValue();
    if (map.getNumDims() != numDims)
      return p->emitError(
          p->getNameLoc(),
          "dim operand count and integer set dim count must match");

    unsigned numDimAndSymbolOperands =
        result->operands.size() - currentNumOperands;
    if (numDims + map.getNumSymbols() != numDimAndSymbolOperands)
      return p->emitError(
          p->getNameLoc(),
          "symbol operand count and integer set symbol count must match");

    // If the map has multiple results, make sure that we parsed the min/max
    // prefix.
    if (map.getNumResults() > 1 && failedToParsedMinMax) {
      if (isLower) {
        return p->emitError(attrLoc, "lower loop bound affine map with "
                                     "multiple results requires 'max' prefix");
      }
      return p->emitError(attrLoc, "upper loop bound affine map with multiple "
                                   "results requires 'min' prefix");
    }
    return success();
  }

  // Parse custom assembly form.
  if (auto integerAttr = boundAttr.dyn_cast<IntegerAttr>()) {
    result->attributes.pop_back();
    result->addAttribute(
        boundAttrName, builder.getAffineMapAttr(
                           builder.getConstantAffineMap(integerAttr.getInt())));
    return success();
  }

  return p->emitError(
      p->getNameLoc(),
      "expected valid affine map representation for loop bounds");
}

ParseResult AffineForOp::parse(OpAsmParser *parser, OperationState *result) {
  auto &builder = parser->getBuilder();
  OpAsmParser::OperandType inductionVariable;
  // Parse the induction variable followed by '='.
  if (parser->parseRegionArgument(inductionVariable) || parser->parseEqual())
    return failure();

  // Parse loop bounds.
  if (parseBound(/*isLower=*/true, result, parser) ||
      parser->parseKeyword("to", " between bounds") ||
      parseBound(/*isLower=*/false, result, parser))
    return failure();

  // Parse the optional loop step, we default to 1 if one is not present.
  if (parser->parseOptionalKeyword("step")) {
    result->addAttribute(
        getStepAttrName(),
        builder.getIntegerAttr(builder.getIndexType(), /*value=*/1));
  } else {
    llvm::SMLoc stepLoc = parser->getCurrentLocation();
    IntegerAttr stepAttr;
    if (parser->parseAttribute(stepAttr, builder.getIndexType(),
                               getStepAttrName().data(), result->attributes))
      return failure();

    if (stepAttr.getValue().getSExtValue() < 0)
      return parser->emitError(
          stepLoc,
          "expected step to be representable as a positive signed integer");
  }

  // Parse the body region.
  Region *body = result->addRegion();
  if (parser->parseRegion(*body, inductionVariable, builder.getIndexType()))
    return failure();

  ensureAffineTerminator(*body, builder, result->location);

  // Parse the optional attribute list.
  if (parser->parseOptionalAttributeDict(result->attributes))
    return failure();

  // Set the operands list as resizable so that we can freely modify the bounds.
  result->setOperandListToResizable();
  return success();
}

static void printBound(AffineMapAttr boundMap,
                       Operation::operand_range boundOperands,
                       const char *prefix, OpAsmPrinter *p) {
  AffineMap map = boundMap.getValue();

  // Check if this bound should be printed using custom assembly form.
  // The decision to restrict printing custom assembly form to trivial cases
  // comes from the will to roundtrip MLIR binary -> text -> binary in a
  // lossless way.
  // Therefore, custom assembly form parsing and printing is only supported for
  // zero-operand constant maps and single symbol operand identity maps.
  if (map.getNumResults() == 1) {
    AffineExpr expr = map.getResult(0);

    // Print constant bound.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 0) {
      if (auto constExpr = expr.dyn_cast<AffineConstantExpr>()) {
        *p << constExpr.getValue();
        return;
      }
    }

    // Print bound that consists of a single SSA symbol if the map is over a
    // single symbol.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 1) {
      if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
        p->printOperand(*boundOperands.begin());
        return;
      }
    }
  } else {
    // Map has multiple results. Print 'min' or 'max' prefix.
    *p << prefix << ' ';
  }

  // Print the map and its operands.
  *p << boundMap;
  printDimAndSymbolList(boundOperands.begin(), boundOperands.end(),
                        map.getNumDims(), p);
}

void AffineForOp::print(OpAsmPrinter *p) {
  *p << "affine.for ";
  p->printOperand(getBody()->getArgument(0));
  *p << " = ";
  printBound(getLowerBoundMapAttr(), getLowerBoundOperands(), "max", p);
  *p << " to ";
  printBound(getUpperBoundMapAttr(), getUpperBoundOperands(), "min", p);

  if (getStep() != 1)
    *p << " step " << getStep();
  p->printRegion(getRegion(),
                 /*printEntryBlockArgs=*/false,
                 /*printBlockTerminators=*/false);
  p->printOptionalAttrDict(getAttrs(),
                           /*elidedAttrs=*/{getLowerBoundAttrName(),
                                            getUpperBoundAttrName(),
                                            getStepAttrName()});
}

namespace {
/// This is a pattern to fold constant loop bounds.
struct AffineForLoopBoundFolder : public OpRewritePattern<AffineForOp> {
  using OpRewritePattern<AffineForOp>::OpRewritePattern;

  PatternMatchResult matchAndRewrite(AffineForOp forOp,
                                     PatternRewriter &rewriter) const override {
    auto foldLowerOrUpperBound = [&forOp](bool lower) {
      // Check to see if each of the operands is the result of a constant.  If
      // so, get the value.  If not, ignore it.
      SmallVector<Attribute, 8> operandConstants;
      auto boundOperands =
          lower ? forOp.getLowerBoundOperands() : forOp.getUpperBoundOperands();
      for (auto *operand : boundOperands) {
        Attribute operandCst;
        matchPattern(operand, m_Constant(&operandCst));
        operandConstants.push_back(operandCst);
      }

      AffineMap boundMap =
          lower ? forOp.getLowerBoundMap() : forOp.getUpperBoundMap();
      assert(boundMap.getNumResults() >= 1 &&
             "bound maps should have at least one result");
      SmallVector<Attribute, 4> foldedResults;
      if (failed(boundMap.constantFold(operandConstants, foldedResults)))
        return failure();

      // Compute the max or min as applicable over the results.
      assert(!foldedResults.empty() &&
             "bounds should have at least one result");
      auto maxOrMin = foldedResults[0].cast<IntegerAttr>().getValue();
      for (unsigned i = 1, e = foldedResults.size(); i < e; i++) {
        auto foldedResult = foldedResults[i].cast<IntegerAttr>().getValue();
        maxOrMin = lower ? llvm::APIntOps::smax(maxOrMin, foldedResult)
                         : llvm::APIntOps::smin(maxOrMin, foldedResult);
      }
      lower ? forOp.setConstantLowerBound(maxOrMin.getSExtValue())
            : forOp.setConstantUpperBound(maxOrMin.getSExtValue());
      return success();
    };

    // Try to fold the lower bound.
    bool folded = false;
    if (!forOp.hasConstantLowerBound())
      folded |= succeeded(foldLowerOrUpperBound(/*lower=*/true));

    // Try to fold the upper bound.
    if (!forOp.hasConstantUpperBound())
      folded |= succeeded(foldLowerOrUpperBound(/*lower=*/false));

    // If any of the bounds were folded we return success.
    if (!folded)
      return matchFailure();
    rewriter.updatedRootInPlace(forOp);
    return matchSuccess();
  }
};
} // end anonymous namespace

void AffineForOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.push_back(llvm::make_unique<AffineForLoopBoundFolder>(context));
}

OpBuilder AffineForOp::getBodyBuilder() {
  Block *body = getBody();
  return OpBuilder(body, std::prev(body->end()));
}

AffineBound AffineForOp::getLowerBound() {
  auto lbMap = getLowerBoundMap();
  return AffineBound(AffineForOp(*this), 0, lbMap.getNumInputs(), lbMap);
}

AffineBound AffineForOp::getUpperBound() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  return AffineBound(AffineForOp(*this), lbMap.getNumInputs(), getNumOperands(),
                     ubMap);
}

void AffineForOp::setLowerBound(ArrayRef<Value *> lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value *, 4> newOperands(lbOperands.begin(), lbOperands.end());

  auto ubOperands = getUpperBoundOperands();
  newOperands.append(ubOperands.begin(), ubOperands.end());
  getOperation()->setOperands(newOperands);

  setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBound(ArrayRef<Value *> ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value *, 4> newOperands(getLowerBoundOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  getOperation()->setOperands(newOperands);

  setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setLowerBoundMap(AffineMap map) {
  auto lbMap = getLowerBoundMap();
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)lbMap;
  setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBoundMap(AffineMap map) {
  auto ubMap = getUpperBoundMap();
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)ubMap;
  setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

bool AffineForOp::hasConstantLowerBound() {
  return getLowerBoundMap().isSingleConstant();
}

bool AffineForOp::hasConstantUpperBound() {
  return getUpperBoundMap().isSingleConstant();
}

int64_t AffineForOp::getConstantLowerBound() {
  return getLowerBoundMap().getSingleConstantResult();
}

int64_t AffineForOp::getConstantUpperBound() {
  return getUpperBoundMap().getSingleConstantResult();
}

void AffineForOp::setConstantLowerBound(int64_t value) {
  setLowerBound({}, AffineMap::getConstantMap(value, getContext()));
}

void AffineForOp::setConstantUpperBound(int64_t value) {
  setUpperBound({}, AffineMap::getConstantMap(value, getContext()));
}

AffineForOp::operand_range AffineForOp::getLowerBoundOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

AffineForOp::operand_range AffineForOp::getUpperBoundOperands() {
  return {operand_begin() + getLowerBoundMap().getNumInputs(), operand_end()};
}

bool AffineForOp::matchingBoundOperandList() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  if (lbMap.getNumDims() != ubMap.getNumDims() ||
      lbMap.getNumSymbols() != ubMap.getNumSymbols())
    return false;

  unsigned numOperands = lbMap.getNumInputs();
  for (unsigned i = 0, e = lbMap.getNumInputs(); i < e; i++) {
    // Compare Value *'s.
    if (getOperand(i) != getOperand(numOperands + i))
      return false;
  }
  return true;
}

/// Returns the induction variable for this loop.
Value *AffineForOp::getInductionVar() { return getBody()->getArgument(0); }

/// Returns if the provided value is the induction variable of a AffineForOp.
bool mlir::isForInductionVar(Value *val) {
  return getForInductionVarOwner(val) != AffineForOp();
}

/// Returns the loop parent of an induction variable. If the provided value is
/// not an induction variable, then return nullptr.
AffineForOp mlir::getForInductionVarOwner(Value *val) {
  auto *ivArg = dyn_cast<BlockArgument>(val);
  if (!ivArg || !ivArg->getOwner())
    return AffineForOp();
  auto *containingInst = ivArg->getOwner()->getParent()->getContainingOp();
  return dyn_cast<AffineForOp>(containingInst);
}

/// Extracts the induction variables from a list of AffineForOps and returns
/// them.
void mlir::extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                                   SmallVectorImpl<Value *> *ivs) {
  ivs->reserve(forInsts.size());
  for (auto forInst : forInsts)
    ivs->push_back(forInst.getInductionVar());
}

//===----------------------------------------------------------------------===//
// AffineIfOp
//===----------------------------------------------------------------------===//

void AffineIfOp::build(Builder *builder, OperationState *result,
                       IntegerSet condition,
                       ArrayRef<Value *> conditionOperands) {
  result->addAttribute(getConditionAttrName(), IntegerSetAttr::get(condition));
  result->addOperands(conditionOperands);

  // Reserve 2 regions, one for the 'then' and one for the 'else' regions.
  result->regions.reserve(2);
  result->addRegion(nullptr);
  result->addRegion(nullptr);
}

LogicalResult AffineIfOp::verify() {
  // Verify that we have a condition attribute.
  auto conditionAttr = getAttrOfType<IntegerSetAttr>(getConditionAttrName());
  if (!conditionAttr)
    return emitOpError("requires an integer set attribute named 'condition'");

  // Verify that there are enough operands for the condition.
  IntegerSet condition = conditionAttr.getValue();
  if (getNumOperands() != condition.getNumOperands())
    return emitOpError("operand count and condition integer set dimension and "
                       "symbol count must match");

  // Verify that the operands are valid dimension/symbols.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getOperands(),
                                           condition.getNumDims())))
    return failure();

  // Verify that the entry of each child region does not have arguments.
  for (auto &region : getOperation()->getRegions()) {
    if (region.empty())
      continue;

    // TODO(riverriddle) We currently do not allow multiple blocks in child
    // regions.
    if (std::next(region.begin()) != region.end())
      return emitOpError("expects only one block per 'then' or 'else' regions");
    if (failed(checkHasAffineTerminator(*this, region.front())))
      return failure();

    for (auto &b : region)
      if (b.getNumArguments() != 0)
        return emitOpError(
            "requires that child entry blocks have no arguments");
  }
  return success();
}

ParseResult AffineIfOp::parse(OpAsmParser *parser, OperationState *result) {
  // Parse the condition attribute set.
  IntegerSetAttr conditionAttr;
  unsigned numDims;
  if (parser->parseAttribute(conditionAttr, getConditionAttrName(),
                             result->attributes) ||
      parseDimAndSymbolList(parser, result->operands, numDims))
    return failure();

  // Verify the condition operands.
  auto set = conditionAttr.getValue();
  if (set.getNumDims() != numDims)
    return parser->emitError(
        parser->getNameLoc(),
        "dim operand count and integer set dim count must match");
  if (numDims + set.getNumSymbols() != result->operands.size())
    return parser->emitError(
        parser->getNameLoc(),
        "symbol operand count and integer set symbol count must match");

  // Create the regions for 'then' and 'else'.  The latter must be created even
  // if it remains empty for the validity of the operation.
  result->regions.reserve(2);
  Region *thenRegion = result->addRegion();
  Region *elseRegion = result->addRegion();

  // Parse the 'then' region.
  if (parser->parseRegion(*thenRegion, {}, {}))
    return failure();
  ensureAffineTerminator(*thenRegion, parser->getBuilder(), result->location);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser->parseOptionalKeyword("else")) {
    if (parser->parseRegion(*elseRegion, {}, {}))
      return failure();
    ensureAffineTerminator(*elseRegion, parser->getBuilder(), result->location);
  }

  // Parse the optional attribute list.
  if (parser->parseOptionalAttributeDict(result->attributes))
    return failure();

  return success();
}

void AffineIfOp::print(OpAsmPrinter *p) {
  auto conditionAttr = getAttrOfType<IntegerSetAttr>(getConditionAttrName());
  *p << "affine.if " << conditionAttr;
  printDimAndSymbolList(operand_begin(), operand_end(),
                        conditionAttr.getValue().getNumDims(), p);
  p->printRegion(getOperation()->getRegion(0),
                 /*printEntryBlockArgs=*/false,
                 /*printBlockTerminators=*/false);

  // Print the 'else' regions if it has any blocks.
  auto &elseRegion = getOperation()->getRegion(1);
  if (!elseRegion.empty()) {
    *p << " else";
    p->printRegion(elseRegion,
                   /*printEntryBlockArgs=*/false,
                   /*printBlockTerminators=*/false);
  }

  // Print the attribute list.
  p->printOptionalAttrDict(getAttrs(),
                           /*elidedAttrs=*/getConditionAttrName());
}

IntegerSet AffineIfOp::getIntegerSet() {
  return getAttrOfType<IntegerSetAttr>(getConditionAttrName()).getValue();
}
void AffineIfOp::setIntegerSet(IntegerSet newSet) {
  setAttr(getConditionAttrName(), IntegerSetAttr::get(newSet));
}

/// Returns the list of 'then' blocks.
Region &AffineIfOp::getThenBlocks() { return getOperation()->getRegion(0); }

/// Returns the list of 'else' blocks.
Region &AffineIfOp::getElseBlocks() { return getOperation()->getRegion(1); }

//===----------------------------------------------------------------------===//
// AffineLoadOp
//===----------------------------------------------------------------------===//

void AffineLoadOp::build(Builder *builder, OperationState *result,
                         AffineMap map, ArrayRef<Value *> operands) {
  // TODO(b/133776335) Check that map operands are loop IVs or symbols.
  result->addOperands(operands);
  if (map)
    result->addAttribute(getMapAttrName(), builder->getAffineMapAttr(map));
  auto memrefType = operands[0]->getType().cast<MemRefType>();
  result->types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(Builder *builder, OperationState *result,
                         Value *memref, ArrayRef<Value *> indices) {
  result->addOperands(memref);
  result->addOperands(indices);
  auto memrefType = memref->getType().cast<MemRefType>();
  auto map = builder->getMultiDimIdentityMap(memrefType.getRank());
  result->addAttribute(getMapAttrName(), builder->getAffineMapAttr(map));
  result->types.push_back(memrefType.getElementType());
}

ParseResult AffineLoadOp::parse(OpAsmParser *parser, OperationState *result) {
  auto &builder = parser->getBuilder();
  auto affineIntTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser->parseOperand(memrefInfo) ||
      parser->parseAffineMapOfSSAIds(mapOperands, mapAttr, getMapAttrName(),
                                     result->attributes) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(memrefInfo, type, result->operands) ||
      parser->resolveOperands(mapOperands, affineIntTy, result->operands) ||
      parser->addTypeToList(type.getElementType(), result->types));
}

void AffineLoadOp::print(OpAsmPrinter *p) {
  *p << "affine.load " << *getMemRef() << '[';
  AffineMapAttr mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    SmallVector<Value *, 2> operands(getIndices());
    p->printAffineMapOfSSAIds(mapAttr, operands);
  }
  *p << ']';
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{getMapAttrName()});
  *p << " : " << getMemRefType();
}

LogicalResult AffineLoadOp::verify() {
  if (getType() != getMemRefType().getElementType())
    return emitOpError("result type must match element type of memref");

  auto mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    AffineMap map = getAttrOfType<AffineMapAttr>(getMapAttrName()).getValue();
    if (map.getNumResults() != getMemRefType().getRank())
      return emitOpError("affine.load affine map num results must equal"
                         " memref rank");
  }

  for (auto *idx : getIndices())
    if (!idx->getType().isIndex())
      return emitOpError("index to load must have 'index' type");
  // TODO(b/133776335) Verify that map operands are loop IVs or symbols.
  return success();
}

void AffineLoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  /// load(memrefcast) -> load
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// AffineStoreOp
//===----------------------------------------------------------------------===//

void AffineStoreOp::build(Builder *builder, OperationState *result,
                          Value *valueToStore, AffineMap map,
                          ArrayRef<Value *> operands) {
  // TODO(b/133776335) Check that map operands are loop IVs or symbols.
  result->addOperands(valueToStore);
  result->addOperands(operands);
  if (map)
    result->addAttribute(getMapAttrName(), builder->getAffineMapAttr(map));
}

void AffineStoreOp::build(Builder *builder, OperationState *result,
                          Value *valueToStore, Value *memref,
                          ArrayRef<Value *> operands) {
  result->addOperands(valueToStore);
  result->addOperands(memref);
  result->addOperands(operands);
  auto memrefType = memref->getType().cast<MemRefType>();
  auto map = builder->getMultiDimIdentityMap(memrefType.getRank());
  result->addAttribute(getMapAttrName(), builder->getAffineMapAttr(map));
}

ParseResult AffineStoreOp::parse(OpAsmParser *parser, OperationState *result) {
  auto affineIntTy = parser->getBuilder().getIndexType();

  MemRefType type;
  OpAsmParser::OperandType storeValueInfo;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser->parseOperand(storeValueInfo) || parser->parseComma() ||
      parser->parseOperand(memrefInfo) ||
      parser->parseAffineMapOfSSAIds(mapOperands, mapAttr, getMapAttrName(),
                                     result->attributes) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(storeValueInfo, type.getElementType(),
                             result->operands) ||
      parser->resolveOperand(memrefInfo, type, result->operands) ||
      parser->resolveOperands(mapOperands, affineIntTy, result->operands));
}

void AffineStoreOp::print(OpAsmPrinter *p) {
  *p << "affine.store " << *getValueToStore();
  *p << ", " << *getMemRef() << '[';
  AffineMapAttr mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    SmallVector<Value *, 2> operands(getIndices());
    p->printAffineMapOfSSAIds(mapAttr, operands);
  }
  *p << ']';
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{getMapAttrName()});
  *p << " : " << getMemRefType();
}

LogicalResult AffineStoreOp::verify() {
  // First operand must have same type as memref element type.
  if (getValueToStore()->getType() != getMemRefType().getElementType())
    return emitOpError("first operand must have same type memref element type");

  auto mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != getMemRefType().getRank())
      return emitOpError("affine.store affine map num results must equal"
                         " memref rank");
  }
  for (auto *idx : getIndices())
    if (!idx->getType().isIndex())
      return emitOpError("index to load must have 'index' type");
  // TODO(b/133776335) Verify that map operands are loop IVs or symbols.
  return success();
}

void AffineStoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  /// load(memrefcast) -> load
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}
