//===- MemRefUtils.h - MemRef transformation utilities ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header file defines prototypes for various transformation utilities for
// the MemRefOps dialect. These are not passes by themselves but are used
// either by passes, optimization sequences, or in turn by other transformation
// utilities.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_DIALECT_MEMREF_UTILS_MEMREFUTILS_H
#define MLIR_DIALECT_MEMREF_UTILS_MEMREFUTILS_H

#include "mlir/Dialect/MemRef/IR/MemRef.h"

namespace mlir {

class MemRefType;

/// A value with a memref type.
using MemrefValue = TypedValue<BaseMemRefType>;

namespace memref {

/// Returns true, if the memref type has static shapes and represents a
/// contiguous chunk of memory.
bool isStaticShapeAndContiguousRowMajor(MemRefType type);

/// For a `memref` with `offset`, `sizes` and `strides`, returns the
/// offset, size, and potentially the size padded at the front to use for the
/// linearized `memref`.
/// - If the linearization is done for emulating load/stores of
///   element type with bitwidth `srcBits` using element type with
///   bitwidth `dstBits`, the linearized offset and size are
///   scaled down by `dstBits`/`srcBits`.
/// - If `indices` is provided, it represents the position in the
///   original `memref` being accessed. The method then returns the
///   index to use in the linearized `memref`. The linearized index
///   is also scaled down by `dstBits`/`srcBits`. If `indices` is not provided
///   0, is returned for the linearized index.
/// - If the size of the load/store is smaller than the linearized memref
///   load/store, the memory region emulated is larger than the actual memory
///   region needed. `intraDataOffset` returns the element offset of the data
///   relevant at the beginning.
struct LinearizedMemRefInfo {
  OpFoldResult linearizedOffset;
  OpFoldResult linearizedSize;
  OpFoldResult intraDataOffset;
};
std::pair<LinearizedMemRefInfo, OpFoldResult> getLinearizedMemRefOffsetAndSize(
    OpBuilder &builder, Location loc, int srcBits, int dstBits,
    OpFoldResult offset, ArrayRef<OpFoldResult> sizes,
    ArrayRef<OpFoldResult> strides, ArrayRef<OpFoldResult> indices = {});

/// For a `memref` with `offset` and `sizes`, returns the
/// offset and size to use for the linearized `memref`, assuming that
/// the strides are computed from a row-major ordering of the sizes;
/// - If the linearization is done for emulating load/stores of
///   element type with bitwidth `srcBits` using element type with
///   bitwidth `dstBits`, the linearized offset and size are
///   scaled down by `dstBits`/`srcBits`.
LinearizedMemRefInfo
getLinearizedMemRefOffsetAndSize(OpBuilder &builder, Location loc, int srcBits,
                                 int dstBits, OpFoldResult offset,
                                 ArrayRef<OpFoldResult> sizes);

/// Track temporary allocations that are never read from. If this is the case
/// it means both the allocations and associated stores can be removed.
void eraseDeadAllocAndStores(RewriterBase &rewriter, Operation *parentOp);

/// Given a set of sizes, return the suffix product.
///
/// When applied to slicing, this is the calculation needed to derive the
/// strides (i.e. the number of linear indices to skip along the (k-1) most
/// minor dimensions to get the next k-slice).
///
/// This is the basis to linearize an n-D offset confined to `[0 ... sizes]`.
///
/// Assuming `sizes` is `[s0, .. sn]`, return the vector<Value>
///   `[s1 * ... * sn, s2 * ... * sn, ..., sn, 1]`.
///
/// It is the caller's responsibility to provide valid OpFoldResult type values
/// and construct valid IR in the end.
///
/// `sizes` elements are asserted to be non-negative.
///
/// Return an empty vector if `sizes` is empty.
///
/// The function emits an IR block which computes suffix product for provided
/// sizes.
SmallVector<OpFoldResult>
computeSuffixProductIRBlock(Location loc, OpBuilder &builder,
                            ArrayRef<OpFoldResult> sizes);
inline SmallVector<OpFoldResult>
computeStridesIRBlock(Location loc, OpBuilder &builder,
                      ArrayRef<OpFoldResult> sizes) {
  return computeSuffixProductIRBlock(loc, builder, sizes);
}

/// Walk up the source chain until an operation that changes/defines the view of
/// memory is found (i.e. skip operations that alias the entire view).
MemrefValue skipFullyAliasingOperations(MemrefValue source);

/// Checks if two (memref) values are the same or statically known to alias
/// the same region of memory.
inline bool isSameViewOrTrivialAlias(MemrefValue a, MemrefValue b) {
  return skipFullyAliasingOperations(a) == skipFullyAliasingOperations(b);
}

/// Walk up the source chain until we find an operation that is not a view of
/// the source memref (i.e. implements ViewLikeOpInterface).
MemrefValue skipViewLikeOps(MemrefValue source);

/// Given the 'indices' of a load/store operation where the memref is a result
/// of a expand_shape op, returns the indices w.r.t to the source memref of the
/// expand_shape op. For example
///
/// %0 = ... : memref<12x42xf32>
/// %1 = memref.expand_shape %0 [[0, 1], [2]]
///    : memref<12x42xf32> into memref<2x6x42xf32>
/// %2 = load %1[%i1, %i2, %i3] : memref<2x6x42xf32
///
/// could be folded into
///
/// %2 = load %0[6 * i1 + i2, %i3] :
///          memref<12x42xf32>
LogicalResult resolveSourceIndicesExpandShape(
    Location loc, PatternRewriter &rewriter,
    memref::ExpandShapeOp expandShapeOp, ValueRange indices,
    SmallVectorImpl<Value> &sourceIndices, bool startsInbounds);

/// Given the 'indices' of a load/store operation where the memref is a result
/// of a collapse_shape op, returns the indices w.r.t to the source memref of
/// the collapse_shape op. For example
///
/// %0 = ... : memref<2x6x42xf32>
/// %1 = memref.collapse_shape %0 [[0, 1], [2]]
///    : memref<2x6x42xf32> into memref<12x42xf32>
/// %2 = load %1[%i1, %i2] : memref<12x42xf32>
///
/// could be folded into
///
/// %2 = load %0[%i1 / 6, %i1 % 6, %i2] :
///          memref<2x6x42xf32>
LogicalResult
resolveSourceIndicesCollapseShape(Location loc, PatternRewriter &rewriter,
                                  memref::CollapseShapeOp collapseShapeOp,
                                  ValueRange indices,
                                  SmallVectorImpl<Value> &sourceIndices);

} // namespace memref
} // namespace mlir

#endif // MLIR_DIALECT_MEMREF_UTILS_MEMREFUTILS_H
