//===- AffineOps.h - MLIR Affine Operations -------------------------------===//
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
// This file defines convenience types for working with Affine operations
// in the MLIR operation set.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_AFFINEOPS_AFFINEOPS_H
#define MLIR_AFFINEOPS_AFFINEOPS_H

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/StandardTypes.h"

namespace mlir {
class AffineBound;
class AffineValueMap;
class FlatAffineConstraints;
class OpBuilder;

/// A utility function to check if a value is defined at the top level of a
/// function. A value defined at the top level is always a valid symbol.
bool isTopLevelSymbol(Value *value);

class AffineOpsDialect : public Dialect {
public:
  AffineOpsDialect(MLIRContext *context);
  static StringRef getDialectNamespace() { return "affine"; }
};

/// The "affine.apply" operation applies an affine map to a list of operands,
/// yielding a single result. The operand list must be the same size as the
/// number of arguments to the affine mapping.  All operands and the result are
/// of type 'Index'. This operation requires a single affine map attribute named
/// "map".  For example:
///
///   %y = "affine.apply" (%x) { map: (d0) -> (d0 + 1) } :
///          (index) -> (index)
///
/// equivalently:
///
///   #map42 = (d0)->(d0+1)
///   %y = affine.apply #map42(%x)
///
class AffineApplyOp : public Op<AffineApplyOp, OpTrait::VariadicOperands,
                                OpTrait::OneResult, OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  /// Builds an affine apply op with the specified map and operands.
  static void build(Builder *builder, OperationState *result, AffineMap map,
                    ArrayRef<Value *> operands);

  /// Returns the affine map to be applied by this operation.
  AffineMap getAffineMap() {
    return getAttrOfType<AffineMapAttr>("map").getValue();
  }

  /// Returns true if the result of this operation can be used as dimension id.
  bool isValidDim();

  /// Returns true if the result of this operation is a symbol.
  bool isValidSymbol();

  static StringRef getOperationName() { return "affine.apply"; }

  // Hooks to customize behavior of this op.
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  OpFoldResult fold(ArrayRef<Attribute> operands);

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// AffineDmaStartOp starts a non-blocking DMA operation that transfers data
/// from a source memref to a destination memref. The source and destination
/// memref need not be of the same dimensionality, but need to have the same
/// elemental type. The operands include the source and destination memref's
/// each followed by its indices, size of the data transfer in terms of the
/// number of elements (of the elemental type of the memref), a tag memref with
/// its indices, and optionally at the end, a stride and a
/// number_of_elements_per_stride arguments. The tag location is used by an
/// AffineDmaWaitOp to check for completion. The indices of the source memref,
/// destination memref, and the tag memref have the same restrictions as any
/// affine.load/store. In particular, index for each memref dimension must be an
/// affine expression of loop induction variables and symbols.
/// The optional stride arguments should be of 'index' type, and specify a
/// stride for the slower memory space (memory space with a lower memory space
/// id), tranferring chunks of number_of_elements_per_stride every stride until
/// %num_elements are transferred. Either both or no stride arguments should be
/// specified. The value of 'num_elements' must be a multiple of
/// 'number_of_elements_per_stride'.
//
// For example, a DmaStartOp operation that transfers 256 elements of a memref
// '%src' in memory space 0 at indices [%i + 3, %j] to memref '%dst' in memory
// space 1 at indices [%k + 7, %l], would be specified as follows:
//
//   %num_elements = constant 256
//   %idx = constant 0 : index
//   %tag = alloc() : memref<1xi32, 4>
//   affine.dma_start %src[%i + 3, %j], %dst[%k + 7, %l], %tag[%idx],
//     %num_elements :
//       memref<40x128xf32, 0>, memref<2x1024xf32, 1>, memref<1xi32, 2>
//
//   If %stride and %num_elt_per_stride are specified, the DMA is expected to
//   transfer %num_elt_per_stride elements every %stride elements apart from
//   memory space 0 until %num_elements are transferred.
//
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%idx], %num_elements,
//     %stride, %num_elt_per_stride : ...
//
// TODO(mlir-team): add additional operands to allow source and destination
// striding, and multiple stride levels (possibly using AffineMaps to specify
// multiple levels of striding).
// TODO(andydavis) Consider replacing src/dst memref indices with view memrefs.
class AffineDmaStartOp : public Op<AffineDmaStartOp, OpTrait::VariadicOperands,
                                   OpTrait::ZeroResult> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result, Value *srcMemRef,
                    AffineMap srcMap, ArrayRef<Value *> srcIndices,
                    Value *destMemRef, AffineMap dstMap,
                    ArrayRef<Value *> destIndices, Value *tagMemRef,
                    AffineMap tagMap, ArrayRef<Value *> tagIndices,
                    Value *numElements, Value *stride = nullptr,
                    Value *elementsPerStride = nullptr);

  /// Returns the operand index of the src memref.
  unsigned getSrcMemRefOperandIndex() { return 0; }

  /// Returns the source MemRefType for this DMA operation.
  Value *getSrcMemRef() { return getOperand(getSrcMemRefOperandIndex()); }
  MemRefType getSrcMemRefType() {
    return getSrcMemRef()->getType().cast<MemRefType>();
  }

  /// Returns the rank (number of indices) of the source MemRefType.
  unsigned getSrcMemRefRank() { return getSrcMemRefType().getRank(); }

  /// Returns the affine map used to access the src memref.
  AffineMap getSrcMap() { return getSrcMapAttr().getValue(); }
  AffineMapAttr getSrcMapAttr() {
    return getAttr(getSrcMapAttrName()).cast<AffineMapAttr>();
  }

  /// Returns the source memref affine map indices for this DMA operation.
  operand_range getSrcIndices() {
    return {operand_begin() + getSrcMemRefOperandIndex() + 1,
            operand_begin() + getSrcMemRefOperandIndex() + 1 +
                getSrcMap().getNumInputs()};
  }

  /// Returns the memory space of the src memref.
  unsigned getSrcMemorySpace() {
    return getSrcMemRef()->getType().cast<MemRefType>().getMemorySpace();
  }

  /// Returns the operand index of the dst memref.
  unsigned getDstMemRefOperandIndex() {
    return getSrcMemRefOperandIndex() + 1 + getSrcMap().getNumInputs();
  }

  /// Returns the destination MemRefType for this DMA operations.
  Value *getDstMemRef() { return getOperand(getDstMemRefOperandIndex()); }
  MemRefType getDstMemRefType() {
    return getDstMemRef()->getType().cast<MemRefType>();
  }

  /// Returns the rank (number of indices) of the destination MemRefType.
  unsigned getDstMemRefRank() {
    return getDstMemRef()->getType().cast<MemRefType>().getRank();
  }

  /// Returns the memory space of the src memref.
  unsigned getDstMemorySpace() {
    return getDstMemRef()->getType().cast<MemRefType>().getMemorySpace();
  }

  /// Returns the affine map used to access the dst memref.
  AffineMap getDstMap() { return getDstMapAttr().getValue(); }
  AffineMapAttr getDstMapAttr() {
    return getAttr(getDstMapAttrName()).cast<AffineMapAttr>();
  }

  /// Returns the destination memref indices for this DMA operation.
  operand_range getDstIndices() {
    return {operand_begin() + getDstMemRefOperandIndex() + 1,
            operand_begin() + getDstMemRefOperandIndex() + 1 +
                getDstMap().getNumInputs()};
  }

  /// Returns the operand index of the tag memref.
  unsigned getTagMemRefOperandIndex() {
    return getDstMemRefOperandIndex() + 1 + getDstMap().getNumInputs();
  }

  /// Returns the Tag MemRef for this DMA operation.
  Value *getTagMemRef() { return getOperand(getTagMemRefOperandIndex()); }
  MemRefType getTagMemRefType() {
    return getTagMemRef()->getType().cast<MemRefType>();
  }

  /// Returns the rank (number of indices) of the tag MemRefType.
  unsigned getTagMemRefRank() {
    return getTagMemRef()->getType().cast<MemRefType>().getRank();
  }

  /// Returns the affine map used to access the tag memref.
  AffineMap getTagMap() { return getTagMapAttr().getValue(); }
  AffineMapAttr getTagMapAttr() {
    return getAttr(getTagMapAttrName()).cast<AffineMapAttr>();
  }

  /// Returns the tag memref indices for this DMA operation.
  operand_range getTagIndices() {
    return {operand_begin() + getTagMemRefOperandIndex() + 1,
            operand_begin() + getTagMemRefOperandIndex() + 1 +
                getTagMap().getNumInputs()};
  }

  /// Returns the number of elements being transferred by this DMA operation.
  Value *getNumElements() {
    return getOperand(getTagMemRefOperandIndex() + 1 +
                      getTagMap().getNumInputs());
  }

  /// Returns the AffineMapAttr associated with 'memref'.
  NamedAttribute getAffineMapAttrForMemRef(Value *memref) {
    if (memref == getSrcMemRef())
      return {Identifier::get(getSrcMapAttrName(), getContext()),
              getSrcMapAttr()};
    else if (memref == getDstMemRef())
      return {Identifier::get(getDstMapAttrName(), getContext()),
              getDstMapAttr()};
    assert(memref == getTagMemRef() &&
           "DmaStartOp expected source, destination or tag memref");
    return {Identifier::get(getTagMapAttrName(), getContext()),
            getTagMapAttr()};
  }

  /// Returns true if this is a DMA from a faster memory space to a slower one.
  bool isDestMemorySpaceFaster() {
    return (getSrcMemorySpace() < getDstMemorySpace());
  }

  /// Returns true if this is a DMA from a slower memory space to a faster one.
  bool isSrcMemorySpaceFaster() {
    // Assumes that a lower number is for a slower memory space.
    return (getDstMemorySpace() < getSrcMemorySpace());
  }

  /// Given a DMA start operation, returns the operand position of either the
  /// source or destination memref depending on the one that is at the higher
  /// level of the memory hierarchy. Asserts failure if neither is true.
  unsigned getFasterMemPos() {
    assert(isSrcMemorySpaceFaster() || isDestMemorySpaceFaster());
    return isSrcMemorySpaceFaster() ? 0 : getDstMemRefOperandIndex();
  }

  static StringRef getSrcMapAttrName() { return "src_map"; }
  static StringRef getDstMapAttrName() { return "dst_map"; }
  static StringRef getTagMapAttrName() { return "tag_map"; }

  static StringRef getOperationName() { return "affine.dma_start"; }
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);

  /// Returns true if this DMA operation is strided, returns false otherwise.
  bool isStrided() {
    return getNumOperands() !=
           getTagMemRefOperandIndex() + 1 + getTagMap().getNumInputs() + 1;
  }

  /// Returns the stride value for this DMA operation.
  Value *getStride() {
    if (!isStrided())
      return nullptr;
    return getOperand(getNumOperands() - 1 - 1);
  }

  /// Returns the number of elements to transfer per stride for this DMA op.
  Value *getNumElementsPerStride() {
    if (!isStrided())
      return nullptr;
    return getOperand(getNumOperands() - 1);
  }
};

/// AffineDmaWaitOp blocks until the completion of a DMA operation associated
/// with the tag element '%tag[%index]'. %tag is a memref, and %index has to be
/// an index with the same restrictions as any load/store index. In particular,
/// index for each memref dimension must be an affine expression of loop
/// induction variables and symbols. %num_elements is the number of elements
/// associated with the DMA operation. For example:
//
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%index], %num_elements :
//     memref<2048xf32, 0>, memref<256xf32, 1>, memref<1xi32, 2>
//   ...
//   ...
//   affine.dma_wait %tag[%index], %num_elements : memref<1xi32, 2>
//
class AffineDmaWaitOp : public Op<AffineDmaWaitOp, OpTrait::VariadicOperands,
                                  OpTrait::ZeroResult> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result, Value *tagMemRef,
                    AffineMap tagMap, ArrayRef<Value *> tagIndices,
                    Value *numElements);

  static StringRef getOperationName() { return "affine.dma_wait"; }

  // Returns the Tag MemRef associated with the DMA operation being waited on.
  Value *getTagMemRef() { return getOperand(0); }
  MemRefType getTagMemRefType() {
    return getTagMemRef()->getType().cast<MemRefType>();
  }

  /// Returns the affine map used to access the tag memref.
  AffineMap getTagMap() { return getTagMapAttr().getValue(); }
  AffineMapAttr getTagMapAttr() {
    return getAttr(getTagMapAttrName()).cast<AffineMapAttr>();
  }

  // Returns the tag memref index for this DMA operation.
  operand_range getTagIndices() {
    return {operand_begin() + 1,
            operand_begin() + 1 + getTagMap().getNumInputs()};
  }

  // Returns the rank (number of indices) of the tag memref.
  unsigned getTagMemRefRank() {
    return getTagMemRef()->getType().cast<MemRefType>().getRank();
  }

  /// Returns the AffineMapAttr associated with 'memref'.
  NamedAttribute getAffineMapAttrForMemRef(Value *memref) {
    assert(memref == getTagMemRef());
    return {Identifier::get(getTagMapAttrName(), getContext()),
            getTagMapAttr()};
  }

  /// Returns the number of elements transferred in the associated DMA op.
  Value *getNumElements() { return getOperand(1 + getTagMap().getNumInputs()); }

  static StringRef getTagMapAttrName() { return "tag_map"; }
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "affine.for" operation represents an affine loop nest, defining an SSA
/// value for its induction variable. It has one region capturing the loop body.
/// The induction variable is represented as a argument of this region. This SSA
/// value always has type index, which is the size of the machine word. The
/// stride, represented by step, is a positive constant integer which defaults
/// to "1" if not present. The lower and upper bounds specify a half-open range:
/// the range includes the lower bound but does not include the upper bound.
///
/// The body region must contain exactly one block that terminates with
/// "affine.terminator".  Calling AffineForOp::build will create such region
/// and insert the terminator, so will the parsing even in cases if it is absent
/// from the custom format.
///
/// The lower and upper bounds of a for operation are represented as an
/// application of an affine mapping to a list of SSA values passed to the map.
/// The same restrictions hold for these SSA values as for all bindings of SSA
/// values to dimensions and symbols. The affine mappings for the bounds may
/// return multiple results, in which case the max/min keywords are required
/// (for the lower/upper bound respectively), and the bound is the
/// maximum/minimum of the returned values.
///
/// Example:
///
///   affine.for %i = 1 to 10 {
///     ...
///   }
///
class AffineForOp
    : public Op<AffineForOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    ArrayRef<Value *> lbOperands, AffineMap lbMap,
                    ArrayRef<Value *> ubOperands, AffineMap ubMap,
                    int64_t step = 1);
  static void build(Builder *builder, OperationState *result, int64_t lb,
                    int64_t ub, int64_t step = 1);
  LogicalResult verify();
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);

  static StringRef getOperationName() { return "affine.for"; }
  static StringRef getStepAttrName() { return "step"; }
  static StringRef getLowerBoundAttrName() { return "lower_bound"; }
  static StringRef getUpperBoundAttrName() { return "upper_bound"; }

  /// Return a Builder set up to insert operations immediately before the
  /// terminator.
  OpBuilder getBodyBuilder();

  /// Get the body of the AffineForOp.
  Block *getBody() { return &getRegion().front(); }

  /// Get the body region of the AffineForOp.
  Region &getRegion() { return getOperation()->getRegion(0); }

  /// Returns the induction variable for this loop.
  Value *getInductionVar();

  //===--------------------------------------------------------------------===//
  // Bounds and step
  //===--------------------------------------------------------------------===//

  // TODO: provide iterators for the lower and upper bound operands
  // if the current access via getLowerBound(), getUpperBound() is too slow.

  /// Returns operands for the lower bound map.
  operand_range getLowerBoundOperands();

  /// Returns operands for the upper bound map.
  operand_range getUpperBoundOperands();

  /// Returns information about the lower bound as a single object.
  AffineBound getLowerBound();

  /// Returns information about the upper bound as a single object.
  AffineBound getUpperBound();

  /// Returns loop step.
  int64_t getStep() {
    return getAttr(getStepAttrName()).cast<IntegerAttr>().getInt();
  }

  /// Returns affine map for the lower bound.
  AffineMap getLowerBoundMap() { return getLowerBoundMapAttr().getValue(); }
  AffineMapAttr getLowerBoundMapAttr() {
    return getAttr(getLowerBoundAttrName()).cast<AffineMapAttr>();
  }
  /// Returns affine map for the upper bound. The upper bound is exclusive.
  AffineMap getUpperBoundMap() { return getUpperBoundMapAttr().getValue(); }
  AffineMapAttr getUpperBoundMapAttr() {
    return getAttr(getUpperBoundAttrName()).cast<AffineMapAttr>();
  }

  /// Set lower bound. The new bound must have the same number of operands as
  /// the current bound map. Otherwise, 'replaceForLowerBound' should be used.
  void setLowerBound(ArrayRef<Value *> operands, AffineMap map);
  /// Set upper bound. The new bound must not have more operands than the
  /// current bound map. Otherwise, 'replaceForUpperBound' should be used.
  void setUpperBound(ArrayRef<Value *> operands, AffineMap map);

  /// Set the lower bound map without changing operands.
  void setLowerBoundMap(AffineMap map);

  /// Set the upper bound map without changing operands.
  void setUpperBoundMap(AffineMap map);

  /// Set loop step.
  void setStep(int64_t step) {
    assert(step > 0 && "step has to be a positive integer constant");
    auto *context = getLowerBoundMap().getContext();
    setAttr(Identifier::get(getStepAttrName(), context),
            IntegerAttr::get(IndexType::get(context), step));
  }

  /// Returns true if the lower bound is constant.
  bool hasConstantLowerBound();
  /// Returns true if the upper bound is constant.
  bool hasConstantUpperBound();
  /// Returns true if both bounds are constant.
  bool hasConstantBounds() {
    return hasConstantLowerBound() && hasConstantUpperBound();
  }
  /// Returns the value of the constant lower bound.
  /// Fails assertion if the bound is non-constant.
  int64_t getConstantLowerBound();
  /// Returns the value of the constant upper bound. The upper bound is
  /// exclusive. Fails assertion if the bound is non-constant.
  int64_t getConstantUpperBound();
  /// Sets the lower bound to the given constant value.
  void setConstantLowerBound(int64_t value);
  /// Sets the upper bound to the given constant value.
  void setConstantUpperBound(int64_t value);

  /// Returns true if both the lower and upper bound have the same operand lists
  /// (same operands in the same order).
  bool matchingBoundOperandList();
};

/// Returns if the provided value is the induction variable of a AffineForOp.
bool isForInductionVar(Value *val);

/// Returns the loop parent of an induction variable. If the provided value is
/// not an induction variable, then return nullptr.
AffineForOp getForInductionVarOwner(Value *val);

/// Extracts the induction variables from a list of AffineForOps and places them
/// in the output argument `ivs`.
void extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                             SmallVectorImpl<Value *> *ivs);

/// AffineBound represents a lower or upper bound in the for operation.
/// This class does not own the underlying operands. Instead, it refers
/// to the operands stored in the AffineForOp. Its life span should not exceed
/// that of the for operation it refers to.
class AffineBound {
public:
  AffineForOp getAffineForOp() { return op; }
  AffineMap getMap() { return map; }

  /// Returns an AffineValueMap representing this bound.
  AffineValueMap getAsAffineValueMap();

  unsigned getNumOperands() { return opEnd - opStart; }
  Value *getOperand(unsigned idx) {
    return op.getOperation()->getOperand(opStart + idx);
  }

  using operand_iterator = AffineForOp::operand_iterator;
  using operand_range = AffineForOp::operand_range;

  operand_iterator operand_begin() { return op.operand_begin() + opStart; }
  operand_iterator operand_end() { return op.operand_begin() + opEnd; }
  operand_range getOperands() { return {operand_begin(), operand_end()}; }

private:
  // 'affine.for' operation that contains this bound.
  AffineForOp op;
  // Start and end positions of this affine bound operands in the list of
  // the containing 'affine.for' operation operands.
  unsigned opStart, opEnd;
  // Affine map for this bound.
  AffineMap map;

  AffineBound(AffineForOp op, unsigned opStart, unsigned opEnd, AffineMap map)
      : op(op), opStart(opStart), opEnd(opEnd), map(map) {}

  friend class AffineForOp;
};

/// The "if" operation represents an if-then-else construct for conditionally
/// executing two regions of code. The operands to an if operation are an
/// IntegerSet condition and a set of symbol/dimension operands to the
/// condition set. The operation produces no results. For example:
///
///    affine.if #set(%i)  {
///      ...
///    } else {
///      ...
///    }
///
/// The 'else' blocks to the if operation are optional, and may be omitted. For
/// example:
///
///    affine.if #set(%i)  {
///      ...
///    }
///
class AffineIfOp
    : public Op<AffineIfOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    IntegerSet condition, ArrayRef<Value *> conditionOperands);

  static StringRef getOperationName() { return "affine.if"; }
  static StringRef getConditionAttrName() { return "condition"; }

  IntegerSet getIntegerSet();
  void setIntegerSet(IntegerSet newSet);

  /// Returns the 'then' region.
  Region &getThenBlocks();

  /// Returns the 'else' blocks.
  Region &getElseBlocks();

  LogicalResult verify();
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
};

/// Affine terminator is a special terminator operation for blocks inside affine
/// loops and branches. It unconditionally transmits the control flow to the
/// successor of the operation enclosing the region.
///
/// This operation does _not_ have a custom syntax. However, affine control
/// operations omit the terminator in their custom syntax for brevity.
class AffineTerminatorOp
    : public Op<AffineTerminatorOp, OpTrait::ZeroOperands, OpTrait::ZeroResult,
                OpTrait::IsTerminator> {
public:
  using Op::Op;

  static void build(Builder *, OperationState *) {}

  static StringRef getOperationName() { return "affine.terminator"; }
};

/// The "affine.load" op reads an element from a memref, where the index
/// for each memref dimension is an affine expression of loop induction
/// variables and symbols. The output of 'affine.load' is a new value with the
/// same type as the elements of the memref. An affine expression of loop IVs
/// and symbols must be specified for each dimension of the memref. The keyword
/// 'symbol' can be used to indicate SSA identifiers which are symbolic.
//
//  Example 1:
//
//    %1 = affine.load %0[%i0 + 3, %i1 + 7] : memref<100x100xf32>
//
//  Example 2: Uses 'symbol' keyword for symbols '%n' and '%m'.
//
//    %1 = affine.load %0[%i0 + symbol(%n), %i1 + symbol(%m)]
//      : memref<100x100xf32>
//
class AffineLoadOp : public Op<AffineLoadOp, OpTrait::OneResult,
                               OpTrait::AtLeastNOperands<1>::Impl> {
public:
  using Op::Op;

  /// Builds an affine load op with the specified map and operands.
  static void build(Builder *builder, OperationState *result, AffineMap map,
                    ArrayRef<Value *> operands);
  /// Builds an affine load op an identify map and operands.
  static void build(Builder *builder, OperationState *result, Value *memref,
                    ArrayRef<Value *> indices = {});

  /// Returns the operand index of the memref.
  unsigned getMemRefOperandIndex() { return 0; }

  /// Get memref operand.
  Value *getMemRef() { return getOperand(getMemRefOperandIndex()); }
  void setMemRef(Value *value) { setOperand(getMemRefOperandIndex(), value); }
  MemRefType getMemRefType() {
    return getMemRef()->getType().cast<MemRefType>();
  }

  /// Get affine map operands.
  operand_range getIndices() { return llvm::drop_begin(getOperands(), 1); }

  /// Returns the affine map used to index the memref for this operation.
  AffineMap getAffineMap() { return getAffineMapAttr().getValue(); }
  AffineMapAttr getAffineMapAttr() {
    return getAttr(getMapAttrName()).cast<AffineMapAttr>();
  }

  /// Returns the AffineMapAttr associated with 'memref'.
  NamedAttribute getAffineMapAttrForMemRef(Value *memref) {
    assert(memref == getMemRef());
    return {Identifier::get(getMapAttrName(), getContext()),
            getAffineMapAttr()};
  }

  static StringRef getMapAttrName() { return "map"; }
  static StringRef getOperationName() { return "affine.load"; }

  // Hooks to customize behavior of this op.
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "affine.store" op writes an element to a memref, where the index
/// for each memref dimension is an affine expression of loop induction
/// variables and symbols. The 'affine.store' op stores a new value which is the
/// same type as the elements of the memref. An affine expression of loop IVs
/// and symbols must be specified for each dimension of the memref. The keyword
/// 'symbol' can be used to indicate SSA identifiers which are symbolic.
//
//  Example 1:
//
//    affine.store %v0, %0[%i0 + 3, %i1 + 7] : memref<100x100xf32>
//
//  Example 2: Uses 'symbol' keyword for symbols '%n' and '%m'.
//
//    affine.store %v0, %0[%i0 + symbol(%n), %i1 + symbol(%m)]
//      : memref<100x100xf32>
//
class AffineStoreOp : public Op<AffineStoreOp, OpTrait::ZeroResult,
                                OpTrait::AtLeastNOperands<1>::Impl> {
public:
  using Op::Op;

  /// Builds an affine store operation with the specified map and operands.
  static void build(Builder *builder, OperationState *result,
                    Value *valueToStore, AffineMap map,
                    ArrayRef<Value *> operands);
  /// Builds an affine store operation with an identity map and operands.
  static void build(Builder *builder, OperationState *result,
                    Value *valueToStore, Value *memref,
                    ArrayRef<Value *> operands);

  /// Get value to be stored by store operation.
  Value *getValueToStore() { return getOperand(0); }

  /// Returns the operand index of the memref.
  unsigned getMemRefOperandIndex() { return 1; }

  /// Get memref operand.
  Value *getMemRef() { return getOperand(getMemRefOperandIndex()); }
  void setMemRef(Value *value) { setOperand(getMemRefOperandIndex(), value); }

  MemRefType getMemRefType() {
    return getMemRef()->getType().cast<MemRefType>();
  }

  /// Get affine map operands.
  operand_range getIndices() { return llvm::drop_begin(getOperands(), 2); }

  /// Returns the affine map used to index the memref for this operation.
  AffineMap getAffineMap() { return getAffineMapAttr().getValue(); }
  AffineMapAttr getAffineMapAttr() {
    return getAttr(getMapAttrName()).cast<AffineMapAttr>();
  }

  /// Returns the AffineMapAttr associated with 'memref'.
  NamedAttribute getAffineMapAttrForMemRef(Value *memref) {
    assert(memref == getMemRef());
    return {Identifier::get(getMapAttrName(), getContext()),
            getAffineMapAttr()};
  }

  static StringRef getMapAttrName() { return "map"; }
  static StringRef getOperationName() { return "affine.store"; }

  // Hooks to customize behavior of this op.
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// Returns true if the given Value can be used as a dimension id.
bool isValidDim(Value *value);

/// Returns true if the given Value can be used as a symbol.
bool isValidSymbol(Value *value);

/// Modifies both `map` and `operands` in-place so as to:
/// 1. drop duplicate operands
/// 2. drop unused dims and symbols from map
void canonicalizeMapAndOperands(AffineMap *map,
                                llvm::SmallVectorImpl<Value *> *operands);

/// Returns a composed AffineApplyOp by composing `map` and `operands` with
/// other AffineApplyOps supplying those operands. The operands of the resulting
/// AffineApplyOp do not change the length of  AffineApplyOp chains.
AffineApplyOp makeComposedAffineApply(OpBuilder &b, Location loc, AffineMap map,
                                      llvm::ArrayRef<Value *> operands);

/// Given an affine map `map` and its input `operands`, this method composes
/// into `map`, maps of AffineApplyOps whose results are the values in
/// `operands`, iteratively until no more of `operands` are the result of an
/// AffineApplyOp. When this function returns, `map` becomes the composed affine
/// map, and each Value in `operands` is guaranteed to be either a loop IV or a
/// terminal symbol, i.e., a symbol defined at the top level or a block/function
/// argument.
void fullyComposeAffineMapAndOperands(AffineMap *map,
                                      llvm::SmallVectorImpl<Value *> *operands);

} // end namespace mlir

#endif
