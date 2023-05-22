/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "mlir/Dialect/Tensor/IR/Tensor.h"

// clang-format erroneously puts the Tensor.h header above.
#include <optional>  // NOLINT

#include "llvm/ADT/STLExtras.h"
#include "tools/mlir_interpreter/dialects/util.h"
#include "tools/mlir_interpreter/framework/interpreter.h"
#include "tools/mlir_interpreter/framework/interpreter_value_util.h"
#include "tools/mlir_interpreter/framework/registration.h"

namespace mlir {
namespace interpreter {
namespace {

int64_t dim(InterpreterState& state, tensor::DimOp,
            const InterpreterValue& tensor, int64_t dim) {
  return dimImpl(tensor, dim, state);
}

InterpreterValue empty(InterpreterState&, tensor::EmptyOp op,
                       ArrayRef<int64_t> dynamicSizes) {
  auto ty = op->getResultTypes().front().cast<mlir::ShapedType>();
  auto shape = replaceDynamicVals(ty.getShape(), dynamicSizes);
  return InterpreterValue::makeTensor(ty.getElementType(), shape);
}

InterpreterValue extract(InterpreterState& state, tensor::ExtractOp,
                         InterpreterValue tensor, ArrayRef<int64_t> indices) {
  if (!tensor.view().inBounds(indices)) {
    state.addFailure("array index out of bounds");
    return {};
  }
  return tensor.extractElement(indices);
}

InterpreterValue fromElements(InterpreterState&, tensor::FromElementsOp op,
                              MutableArrayRef<InterpreterValue> elements) {
  auto ty = op->getResultTypes().front().cast<mlir::ShapedType>();
  auto result = InterpreterValue::makeTensor(ty.getElementType(),
                                             llvm::to_vector(ty.getShape()));
  for (auto [index, element] : llvm::zip(result.view().indices(), elements)) {
    result.insertElement(index, element);
  }
  return result;
}

llvm::SmallVector<InterpreterValue> collapseShape(
    InterpreterState&, tensor::CollapseShapeOp op,
    const InterpreterValue& tensor) {
  SmallVector<int64_t> sizes;
  for (const auto& indices : op.getReassociationIndices()) {
    int64_t size = 1;
    for (auto dim : indices) {
      size *= tensor.view().sizes[dim];
    }
    sizes.push_back(size);
  }
  return {reshapeTensor(tensor, sizes)};
}

llvm::SmallVector<InterpreterValue> expandShape(
    InterpreterState&, tensor::ExpandShapeOp op,
    const InterpreterValue& tensor) {
  auto ty = op->getResultTypes().front().template cast<mlir::ShapedType>();
  auto sizes = llvm::to_vector(ty.getShape());
  for (const auto& [srcIndex, dstIndices] :
       llvm::enumerate(op.getReassociationIndices())) {
    int64_t size = tensor.view().sizes[srcIndex];
    std::optional<int64_t> dynIndex = std::nullopt;
    for (auto dim : dstIndices) {
      if (sizes[dim] < 0) {
        dynIndex = dim;
      } else {
        size /= sizes[dim];
      }
    }
    if (dynIndex) {
      sizes[*dynIndex] = size;
    }
  }
  return {reshapeTensor(tensor, sizes)};
}

llvm::SmallVector<InterpreterValue> extractSlice(
    InterpreterState&, tensor::ExtractSliceOp extract, InterpreterValue tensor,
    ArrayRef<int64_t> dynamicOffsets, ArrayRef<int64_t> dynamicSizes,
    ArrayRef<int64_t> dynamicStrides) {
  auto v = extractOffsetsSizesStrides(dynamicOffsets, dynamicSizes,
                                      dynamicStrides, extract);
  int64_t rank = v.offsets.size();
  auto out = tensor.typedAlike(v.sizes);
  out.fill([&](llvm::ArrayRef<int64_t> indices) {
    llvm::SmallVector<int64_t> srcIndices;
    for (int64_t i = 0; i < rank; ++i) {
      srcIndices.push_back(indices[i] * v.strides[i] + v.offsets[i]);
    }
    return tensor.extractElement(srcIndices);
  });

  int64_t numDropped = 0;
  auto& outView = out.view();

  // TODO(jreiffers): Figure out why getDroppedDims fails here when there's
  // no rank reduction and the output has a dynamic shape.
  int64_t dim = 0;
  const auto& resultSizes = extract.getResultType().getShape();
  const auto& staticSizes = extract.getStaticSizes();
  while (dim < outView.rank()) {
    if (staticSizes[numDropped + dim] == 1 &&
        (dim >= resultSizes.size() || resultSizes[dim] != 1)) {
      outView.sizes.erase(outView.sizes.begin() + dim);
      outView.strides.erase(outView.strides.begin() + dim);
      ++numDropped;
    } else {
      ++dim;
    }
  }

  return {out};
}

template <typename Op>
llvm::SmallVector<InterpreterValue> insertSlice(
    InterpreterState&, Op insert, InterpreterValue src, InterpreterValue dest,
    ArrayRef<int64_t> dynamicOffsets, ArrayRef<int64_t> dynamicSizes,
    ArrayRef<int64_t> dynamicStrides) {
  // parallel_insert_slice actually writes to its destination.
  if (insert->getNumResults() == 1) {
    dest = dest.clone();
  }
  auto v = extractOffsetsSizesStrides(dynamicOffsets, dynamicSizes,
                                      dynamicStrides, insert);

  auto staticSizes = insert.getStaticSizes();
  llvm::SmallVector<int64_t> insertedDims;
  auto& srcView = src.view();
  auto* srcSizeIt = srcView.sizes.begin();
  for (auto [dim, size] : llvm::enumerate(staticSizes)) {
    if (srcSizeIt == srcView.sizes.end() || (*srcSizeIt != size && size >= 0)) {
      assert(size == 1 && "Can only insert unit dims");
      insertedDims.push_back(dim);
    } else {
      ++srcSizeIt;
    }
  }

  for (const auto& srcIndices : srcView.indices()) {
    llvm::SmallVector<int64_t> srcWithInsertedDims = srcIndices;
    for (int64_t dim : insertedDims) {
      srcWithInsertedDims.insert(srcWithInsertedDims.begin() + dim, 0);
    }
    llvm::SmallVector<int64_t> dstIndices;
    for (auto [srcIndex, stride, offset] :
         llvm::zip(srcWithInsertedDims, v.strides, v.offsets)) {
      dstIndices.push_back(srcIndex * stride + offset);
    }
    dest.insertElement(dstIndices, src.extractElement(srcIndices));
  }
  if (insert->getNumResults() == 1) {
    return {dest};
  }
  return {};
}

InterpreterValue generate(InterpreterState& state, tensor::GenerateOp generate,
                          ArrayRef<int64_t> dynamicSizes) {
  auto ty = generate->getResultTypes().front().cast<ShapedType>();
  auto sizes = replaceDynamicVals(ty.getShape(), dynamicSizes);

  auto result = InterpreterValue::makeTensor(ty.getElementType(), sizes);
  result.fill([&](ArrayRef<int64_t> indices) -> InterpreterValue {
    auto values = interpret(state, generate.getRegion(),
                            packInterpreterValues<int64_t>(indices));
    if (state.hasFailure()) {
      return {result.extractElement(indices)};
    }
    return values.front();
  });
  return {result};
}

InterpreterValue insert(InterpreterState& state, tensor::InsertOp,
                        const InterpreterValue& value,
                        const InterpreterValue& tensor,
                        ArrayRef<int64_t> indices) {
  auto result = tensor.clone();
  if (result.view().inBounds(indices)) {
    result.insertElement(indices, value);
  } else {
    state.addFailure("array index out of bounds");
  }
  return result;
}

InterpreterValue pad(InterpreterState& state, tensor::PadOp pad,
                     InterpreterValue tensor, ArrayRef<int64_t> dynamicLows,
                     ArrayRef<int64_t> dynamicHighs) {
  auto lows = replaceDynamicVals(pad.getStaticLow(), dynamicLows);
  auto highs = replaceDynamicVals(pad.getStaticHigh(), dynamicHighs);

  auto& view = tensor.view();
  llvm::SmallVector<int64_t> resultSizes;
  for (auto [size, low, high] : llvm::zip(view.sizes, lows, highs)) {
    resultSizes.push_back(size + low + high);
  }

  auto result = tensor.typedAlike(resultSizes);
  result.fill([&](llvm::ArrayRef<int64_t> outIndex) -> InterpreterValue {
    llvm::SmallVector<int64_t> inIndex;
    for (auto [index, low] : llvm::zip(outIndex, lows)) {
      inIndex.push_back(index - low);
    }
    if (view.inBounds(inIndex)) {
      return tensor.extractElement(inIndex);
    }
    return interpret(state, pad.getRegion(), packInterpreterValues(outIndex))
        .front();
  });
  return result;
}

REGISTER_MLIR_INTERPRETER_OP("tensor.cast",
                             "builtin.unrealized_conversion_cast");
REGISTER_MLIR_INTERPRETER_OP("tensor.yield", noOpTerminator);
REGISTER_MLIR_INTERPRETER_OP(collapseShape);
REGISTER_MLIR_INTERPRETER_OP(dim);
REGISTER_MLIR_INTERPRETER_OP(empty);
REGISTER_MLIR_INTERPRETER_OP(expandShape);
REGISTER_MLIR_INTERPRETER_OP(extract);
REGISTER_MLIR_INTERPRETER_OP(extractSlice);
REGISTER_MLIR_INTERPRETER_OP(fromElements);
REGISTER_MLIR_INTERPRETER_OP(generate);
REGISTER_MLIR_INTERPRETER_OP(insert);
REGISTER_MLIR_INTERPRETER_OP(insertSlice<tensor::InsertSliceOp>);
REGISTER_MLIR_INTERPRETER_OP(insertSlice<tensor::ParallelInsertSliceOp>);
REGISTER_MLIR_INTERPRETER_OP(pad);

}  // namespace
}  // namespace interpreter
}  // namespace mlir
