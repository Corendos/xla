/* Copyright 2024 The OpenXLA Authors.

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

#include "xla/backends/cpu/runtime/dot_thunk.h"

#include <complex>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "xla/backends/cpu/runtime/dot_lib.h"
#include "xla/backends/cpu/runtime/thunk.h"
#include "xla/layout_util.h"
#include "xla/primitive_util.h"
#include "xla/service/buffer_assignment.h"
#include "xla/shape.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/tsl/concurrency/async_value_ref.h"
#include "xla/types.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"
#include "tsl/profiler/lib/traceme.h"

namespace xla::cpu {
namespace {

// Dot operation is implemented as a matrix-matrix multiply (row-major x
// rowm-major or col-major x col-major). For batched dot operations, it is
// implemented as multiple matrix multiplications repeated for each batch
// element.
//
// We rely on col-major Eigen contraction and figure out how to represent dot
// operation as a contraction based on the dot dimension numbers.
struct MatMulDims {
  // The number of rows in the LHS.
  int64_t m;

  // The number of columns in the LHS, which also must be equal to the
  // number of rows in the RHS.
  int64_t k;

  // The number of columns in the RHS.
  int64_t n;

  // True if the LHS matrix is column major.
  bool lhs_column_major;

  // True if the LHS contraction dimension is 1.
  bool lhs_canonical;

  // True if the RHS matrix is column major.
  bool rhs_column_major;

  // True if the RHS contraction dimension is 0.
  bool rhs_canonical;
};

}  // namespace

static MatMulDims GetMatMulDims(
    const Shape& lhs_shape, absl::Span<const int64_t> lhs_contracting_dims,
    const Shape& rhs_shape, absl::Span<const int64_t> rhs_contracting_dims) {
  // Non-contracting dots should never make it here.
  CHECK_EQ(lhs_contracting_dims.size(), 1);
  CHECK_EQ(rhs_contracting_dims.size(), 1);
  CHECK_LT(lhs_contracting_dims[0], 2);
  CHECK_LT(rhs_contracting_dims[0], 2);

  auto is_column_major = [](const Shape& shape) {
    return shape.rank() > 1 && LayoutUtil::Minor(shape.layout(), 0) == 0;
  };

  return MatMulDims{
      /*m=*/lhs_shape.rank() <= 1
          ? 1LL
          : lhs_shape.dimensions(1LL - lhs_contracting_dims[0]),
      /*k=*/lhs_shape.dimensions(lhs_contracting_dims[0]),
      /*n=*/rhs_shape.rank() <= 1
          ? 1LL
          : rhs_shape.dimensions(1LL - rhs_contracting_dims[0]),
      /*lhs_column_major=*/is_column_major(lhs_shape),
      /*lhs_canonical=*/lhs_shape.rank() <= 1 || lhs_contracting_dims[0] == 1,
      /*rhs_column_major=*/is_column_major(rhs_shape),
      /*rhs_canonical=*/rhs_contracting_dims[0] == 0};
}

absl::StatusOr<std::unique_ptr<DotThunk>> DotThunk::Create(
    Info info, DotDimensionNumbers dot_dimensions,
    BufferAllocation::Slice lhs_buffer, Shape lhs_shape,
    BufferAllocation::Slice rhs_buffer, Shape rhs_shape,
    BufferAllocation::Slice out_buffer, Shape out_shape) {
  TF_ASSIGN_OR_RETURN(DotShape dot_shape, GetDotShape(dot_dimensions, lhs_shape,
                                                      rhs_shape, out_shape));

  DotSlices dot_slices{lhs_buffer, std::move(lhs_shape),
                       rhs_buffer, std::move(rhs_shape),
                       out_buffer, std::move(out_shape)};

  return absl::WrapUnique(new DotThunk(info, std::move(dot_dimensions),
                                       std::move(dot_slices),
                                       std::move(dot_shape)));
}

DotThunk::DotThunk(Info info, DotDimensionNumbers dot_dimensions,
                   DotSlices dot_slices, DotShape dot_shape)
    : Thunk(Kind::kDot, info),
      dot_dimensions_(std::move(dot_dimensions)),
      dot_slices_(std::move(dot_slices)),
      dot_shape_(std::move(dot_shape)) {
  // Copy from the original dot dimension numbers.
  lhs_matmul_contracting_dims_.assign(
      dot_dimensions_.lhs_contracting_dimensions().begin(),
      dot_dimensions_.lhs_contracting_dimensions().end());
  rhs_matmul_contracting_dims_.assign(
      dot_dimensions_.rhs_contracting_dimensions().begin(),
      dot_dimensions_.rhs_contracting_dimensions().end());

  // Adjust contracting dimensions for leading batch dimensions.
  for (int64_t& dim : lhs_matmul_contracting_dims_)
    dim -= dot_dimensions_.lhs_batch_dimensions_size();
  for (int64_t& dim : rhs_matmul_contracting_dims_)
    dim -= dot_dimensions_.rhs_batch_dimensions_size();
}

tsl::AsyncValueRef<DotThunk::ExecuteEvent> DotThunk::Execute(
    const ExecuteParams& params) {
  tsl::profiler::TraceMe trace([&] { return TraceMeEncode(); });

  TF_ASSIGN_OR_RETURN(
      se::DeviceMemoryBase lhs_data,
      params.buffer_allocations->GetDeviceAddress(dot_slices_.lhs_buffer));

  TF_ASSIGN_OR_RETURN(
      se::DeviceMemoryBase rhs_data,
      params.buffer_allocations->GetDeviceAddress(dot_slices_.rhs_buffer));

  TF_ASSIGN_OR_RETURN(
      se::DeviceMemoryBase out_data,
      params.buffer_allocations->GetDeviceAddress(dot_slices_.out_buffer));

  VLOG(3) << absl::StreamFormat(
      "Dot operation: lhs_batch_dims=[%s], rhs_batch_dims=[%s], "
      "lhs_contract_dims=[%s], rhs_contract_dims=[%s]",
      absl::StrJoin(dot_dimensions_.lhs_batch_dimensions(), ","),
      absl::StrJoin(dot_dimensions_.rhs_batch_dimensions(), ","),
      absl::StrJoin(dot_dimensions_.lhs_contracting_dimensions(), ","),
      absl::StrJoin(dot_dimensions_.rhs_contracting_dimensions(), ","));

  VLOG(3) << absl::StreamFormat(
      "  lhs: %s in slice %s (%p)", dot_slices_.lhs_shape.ToString(true),
      dot_slices_.lhs_buffer.ToString(), lhs_data.opaque());
  VLOG(3) << absl::StreamFormat(
      "  rhs: %s in slice %s (%p)", dot_slices_.rhs_shape.ToString(true),
      dot_slices_.rhs_buffer.ToString(), rhs_data.opaque());
  VLOG(3) << absl::StreamFormat(
      "  out: %s in slice %s (%p)", dot_slices_.out_shape.ToString(true),
      dot_slices_.out_buffer.ToString(), out_data.opaque());

  VLOG(3) << absl::StreamFormat(
      "  matmul shape: batch_size=%d, lhs=%s, rhs=%s, out=%s",
      dot_shape_.batch_size, dot_shape_.lhs_matmul_shape.ToString(true),
      dot_shape_.rhs_matmul_shape.ToString(true),
      dot_shape_.out_matmul_shape.ToString(true));

  MatMulDims matmul_dims =
      GetMatMulDims(dot_shape_.lhs_matmul_shape, lhs_matmul_contracting_dims_,
                    dot_shape_.rhs_matmul_shape, rhs_matmul_contracting_dims_);

  VLOG(3) << absl::StreamFormat(
      "  matmul dims: m=%d, k=%d, n=%d, lhs_column_major=%v, lhs_canonical=%v, "
      "rhs_column_major=%v, rhs_canonical=%v",
      matmul_dims.m, matmul_dims.k, matmul_dims.n, matmul_dims.lhs_column_major,
      matmul_dims.lhs_canonical, matmul_dims.rhs_column_major,
      matmul_dims.rhs_canonical);

  if (params.intra_op_threadpool == nullptr) {
    return InvalidArgument("Intra-op threadpool must be provided for DotThunk");
  }

  // Eigen expects column-major layout. If the matrices are row major, then use
  // the following identity to compute the product:
  //
  //   (A x B)^T = B^T x A^T
  //
  // The connection between this identity and memory layout is that the
  // transpose operation can also be considered as an operation that changes the
  // memory layout of a matrix from row-major to column-major or vice versa.
  //
  // Effectively this involves swapping the 'lhs' with 'rhs' and 'm' with 'n'.

  void* out = out_data.opaque();
  void* lhs = lhs_data.opaque();
  void* rhs = rhs_data.opaque();

  bool transpose_lhs = !matmul_dims.lhs_canonical;
  bool transpose_rhs = !matmul_dims.rhs_canonical;

  CHECK_EQ(matmul_dims.lhs_column_major, matmul_dims.rhs_column_major);
  if (!matmul_dims.lhs_column_major) {
    std::swap(matmul_dims.m, matmul_dims.n);
    std::swap(lhs, rhs);
    std::swap(transpose_lhs, transpose_rhs);
  }

  PrimitiveType element_type = dot_shape_.lhs_matmul_shape.element_type();
  int64_t byte_width = primitive_util::ByteWidth(element_type);

  int64_t lhs_stride = matmul_dims.m * matmul_dims.k * byte_width;
  int64_t rhs_stride = matmul_dims.k * matmul_dims.n * byte_width;
  int64_t out_stride = matmul_dims.m * matmul_dims.n * byte_width;

  auto batch_ptr = [&](void* ptr, int64_t stride, int64_t index) -> void* {
    return static_cast<uint8_t*>(ptr) + stride * index;
  };

  tsl::CountDownAsyncValueRef<ExecuteEvent> state(dot_shape_.batch_size);

  auto dispatch = [&](auto type_tag) {
    for (int64_t i = 0; i < dot_shape_.batch_size; ++i) {
      TypedMatMul<decltype(type_tag)>(
          params.intra_op_threadpool, batch_ptr(out, out_stride, i),
          batch_ptr(lhs, lhs_stride, i), batch_ptr(rhs, rhs_stride, i),
          matmul_dims.m, matmul_dims.n, matmul_dims.k, transpose_lhs,
          transpose_rhs, [state]() mutable { state.CountDown(); });
    }
  };

  switch (element_type) {
    case F16:
      dispatch(half{});
      break;
    case F32:
      dispatch(float{});
      break;
    case F64:
      dispatch(double{});
      break;
    case S32:
      dispatch(int32_t{});
      break;
    case C64:
      dispatch(std::complex<float>{});
      break;
    case C128:
      dispatch(std::complex<double>{});
      break;
    default:
      return Unimplemented(
          "Unsupported element type for DotThunk::Execute: %s",
          primitive_util::LowercasePrimitiveTypeName(element_type));
  }

  return state.AsRef();
}

}  // namespace xla::cpu
