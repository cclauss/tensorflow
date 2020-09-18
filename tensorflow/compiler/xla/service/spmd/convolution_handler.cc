/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/spmd/convolution_handler.h"

#include "absl/algorithm/container.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/dot_as_convolution_util.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_sharding.h"
#include "tensorflow/compiler/xla/service/hlo_sharding_util.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/service/spmd/spmd_partitioner.h"
#include "tensorflow/compiler/xla/service/spmd/spmd_partitioner_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/window_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/numbers.h"

namespace xla {
namespace spmd {

namespace {

// Partition convolution with batch group count.
StatusOr<HloInstruction*> PartitionConvolutionWithBatchGroupCount(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, int64 num_partitions, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);
  if (original_hlo->batch_group_count() == 1 ||
      original_hlo->batch_group_count() < num_partitions) {
    return nullptr;
  }

  const auto& dnums = original_hlo->convolution_dimension_numbers();
  // Only supports batch_group_size equals input_batch_size case.
  const int64 input_batch_size =
      lhs.base_shape().dimensions(dnums.input_batch_dimension());
  const int64 kernel_output_feature_size =
      rhs.base_shape().dimensions(dnums.kernel_output_feature_dimension());
  if (input_batch_size != kernel_output_feature_size ||
      original_hlo->batch_group_count() != input_batch_size) {
    return nullptr;
  }

  // Map RHS indices to LHS indices.
  std::vector<int64> rhs_to_lhs_indices(output_base_shape.rank());
  rhs_to_lhs_indices[dnums.kernel_output_feature_dimension()] =
      dnums.input_batch_dimension();
  rhs_to_lhs_indices[dnums.kernel_input_feature_dimension()] =
      dnums.input_feature_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    rhs_to_lhs_indices[dnums.kernel_spatial_dimensions(i)] =
        dnums.input_spatial_dimensions(i);
  }

  // Map LHS indices to RHS indices.
  std::vector<int64> lhs_to_rhs_indices(output_base_shape.rank());
  for (int64 i = 0; i < rhs_to_lhs_indices.size(); ++i) {
    lhs_to_rhs_indices[rhs_to_lhs_indices[i]] = i;
  }

  // Map LHS indices to output indices.
  std::vector<int64> lhs_to_output_indices(lhs.base_shape().rank(), -1);
  lhs_to_output_indices[dnums.input_batch_dimension()] =
      dnums.output_feature_dimension();
  lhs_to_output_indices[dnums.input_feature_dimension()] =
      dnums.output_batch_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    lhs_to_output_indices[dnums.input_spatial_dimensions(i)] =
        dnums.output_spatial_dimensions(i);
  }

  // Align LHS or RHS to other operand if input batch dim or kernel output
  // feature dim is partitioned.
  auto aligned_rhs_sharding =
      hlo_sharding_util::TransposeSharding(lhs.sharding(), rhs_to_lhs_indices);
  auto aligned_lhs_sharding =
      hlo_sharding_util::TransposeSharding(rhs.sharding(), lhs_to_rhs_indices);

  bool lhs_batch_dim_is_partitioned =
      (ShardCountAtDim(lhs.sharding(), dnums.input_batch_dimension()) ==
       num_partitions);
  bool rhs_output_feature_dim_is_partitioned =
      (ShardCountAtDim(rhs.sharding(),
                       dnums.kernel_output_feature_dimension()) ==
       num_partitions);
  if (!lhs_batch_dim_is_partitioned && !rhs_output_feature_dim_is_partitioned) {
    return nullptr;
  }
  // Reshard LHS or RHS to partition at batch dimension or output feature
  // dimension as the other operand.
  if (lhs_batch_dim_is_partitioned) {
    rhs = rhs.Reshard(aligned_rhs_sharding);
  } else {
    lhs = lhs.Reshard(aligned_lhs_sharding);
  }
  // Align output sharding after LHS and RHS sharding are consistent.
  auto aligned_output_sharding = hlo_sharding_util::TransposeSharding(
      lhs.sharding(), lhs_to_output_indices);

  // Get LHS and RHS sharded shape.
  auto lhs_shard_shape = MakePartitionedShape(lhs.base_shape(), lhs.sharding());
  auto rhs_shard_shape = MakePartitionedShape(rhs.base_shape(), rhs.sharding());
  const int64 batch_group_count =
      CeilOfRatio(original_hlo->batch_group_count(), num_partitions);
  // Create partitioned convolution.
  TF_ASSIGN_OR_RETURN(
      Shape sharded_conv_shape,
      ShapeInference::InferConvolveShape(
          lhs_shard_shape, rhs_shard_shape, original_hlo->feature_group_count(),
          batch_group_count, conv_window, dnums));
  auto sharded_conv = b->AddInstruction(HloInstruction::CreateConvolve(
      sharded_conv_shape, lhs.hlo(), rhs.hlo(),
      original_hlo->feature_group_count(), batch_group_count, conv_window,
      dnums, original_hlo->precision_config()));
  sharded_conv->set_sharding(aligned_output_sharding);
  return PartitionedHlo(sharded_conv, output_base_shape, lhs.state())
      .Reshard(output_sharding)
      .hlo();
}

// Partition convolution with feature group count.
StatusOr<HloInstruction*> PartitionConvolutionWithFeatureGroupCount(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, int64 num_partitions, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);
  if (original_hlo->feature_group_count() == 1 ||
      original_hlo->feature_group_count() < num_partitions) {
    return nullptr;
  }

  const auto& dnums = original_hlo->convolution_dimension_numbers();
  const int64 input_feature_size =
      lhs.base_shape().dimensions(dnums.input_feature_dimension());
  const int64 kernel_output_feature_size =
      rhs.base_shape().dimensions(dnums.kernel_output_feature_dimension());
  if (input_feature_size != kernel_output_feature_size ||
      input_feature_size % original_hlo->feature_group_count() != 0) {
    return nullptr;
  }

  // Align RHS indices to LHS.
  std::vector<int64> rhs_to_lhs_indices(output_base_shape.rank());
  rhs_to_lhs_indices[dnums.kernel_output_feature_dimension()] =
      dnums.input_feature_dimension();
  rhs_to_lhs_indices[dnums.kernel_input_feature_dimension()] =
      dnums.input_batch_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    rhs_to_lhs_indices[dnums.kernel_spatial_dimensions(i)] =
        dnums.input_spatial_dimensions(i);
  }

  // Align LHS indices to RHS.
  std::vector<int64> lhs_to_rhs_indices(output_base_shape.rank());
  for (int64 i = 0; i < rhs_to_lhs_indices.size(); ++i) {
    lhs_to_rhs_indices[rhs_to_lhs_indices[i]] = i;
  }

  // Align LHS indices to output.
  std::vector<int64> lhs_to_output_indices(output_base_shape.rank());
  lhs_to_output_indices[dnums.input_feature_dimension()] =
      dnums.output_feature_dimension();
  lhs_to_output_indices[dnums.input_batch_dimension()] =
      dnums.output_batch_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    lhs_to_output_indices[dnums.input_spatial_dimensions(i)] =
        dnums.output_spatial_dimensions(i);
  }

  // Align LHS or RHS if input_feature_dim or kernel_output_feature_dim is
  // partitioned.
  auto aligned_rhs_sharding =
      hlo_sharding_util::TransposeSharding(lhs.sharding(), rhs_to_lhs_indices);
  auto aligned_lhs_sharding =
      hlo_sharding_util::TransposeSharding(rhs.sharding(), lhs_to_rhs_indices);

  bool lhs_feature_dim_is_partitioned =
      (ShardCountAtDim(lhs.sharding(), dnums.input_feature_dimension()) ==
       num_partitions);
  bool rhs_output_feature_dim_is_partitioned =
      (ShardCountAtDim(rhs.sharding(),
                       dnums.kernel_output_feature_dimension()) ==
       num_partitions);
  if (!lhs_feature_dim_is_partitioned &&
      !rhs_output_feature_dim_is_partitioned) {
    return nullptr;
  }
  // Reshard LHS or RHS to partition at input feature dimension or output
  // feature dimension as the other operand.
  if (lhs_feature_dim_is_partitioned) {
    rhs = rhs.Reshard(aligned_rhs_sharding);
  } else {
    lhs = lhs.Reshard(aligned_lhs_sharding);
  }

  // Align output sharding after LHS and RHS sharding are consistent.
  auto aligned_output_sharding = hlo_sharding_util::TransposeSharding(
      lhs.sharding(), lhs_to_output_indices);

  auto lhs_shard_shape = MakePartitionedShape(lhs.base_shape(), lhs.sharding());
  auto rhs_shard_shape = MakePartitionedShape(rhs.base_shape(), rhs.sharding());
  int64 feature_group_count =
      CeilOfRatio(original_hlo->feature_group_count(), num_partitions);

  TF_ASSIGN_OR_RETURN(
      Shape sharded_conv_shape,
      ShapeInference::InferConvolveShape(
          lhs_shard_shape, rhs_shard_shape, feature_group_count,
          original_hlo->batch_group_count(), conv_window, dnums));
  auto sharded_conv = b->AddInstruction(HloInstruction::CreateConvolve(
      sharded_conv_shape, lhs.hlo(), rhs.hlo(), feature_group_count,
      original_hlo->batch_group_count(), conv_window, dnums,
      original_hlo->precision_config()));
  sharded_conv->set_sharding(aligned_output_sharding);
  return PartitionedHlo(sharded_conv, output_base_shape, lhs.state())
      .Reshard(output_sharding)
      .hlo();
}

// Partition convolution when both LHS and RHS are partitioned at spatial
// dimensions. Halo exchange will happen on RHS only.
StatusOr<HloInstruction*>
PartitionConvolutionWithSpatialDimensionHaloExchangeOnRHS(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, HloInstruction* partition_id,
    HloModule* module, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);
  TF_RET_CHECK(!lhs.sharding().IsTileMaximal() &&
               !rhs.sharding().IsTileMaximal());

  const auto& dnums = original_hlo->convolution_dimension_numbers();
  std::vector<int64> rhs_to_lhs_indices(output_base_shape.rank());
  rhs_to_lhs_indices[dnums.kernel_output_feature_dimension()] =
      dnums.input_batch_dimension();
  rhs_to_lhs_indices[dnums.kernel_input_feature_dimension()] =
      dnums.input_feature_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    rhs_to_lhs_indices[dnums.kernel_spatial_dimensions(i)] =
        dnums.input_spatial_dimensions(i);
  }
  std::vector<int64> lhs_to_rhs_indices(output_base_shape.rank());
  for (int64 i = 0; i < rhs_to_lhs_indices.size(); ++i) {
    lhs_to_rhs_indices[rhs_to_lhs_indices[i]] = i;
  }
  auto aligned_rhs_sharding =
      hlo_sharding_util::TransposeSharding(lhs.sharding(), rhs_to_lhs_indices);
  auto aligned_lhs_sharding =
      hlo_sharding_util::TransposeSharding(rhs.sharding(), lhs_to_rhs_indices);

  auto unsupported_sharding = [&](const HloSharding& lhs_sharding,
                                  const HloSharding& rhs_sharding) {
    // We currently don't support partitioning input batch or output feature
    // dimensions.
    return lhs_sharding.tile_assignment().dim(dnums.input_batch_dimension()) !=
               1 ||
           rhs_sharding.tile_assignment().dim(
               dnums.kernel_output_feature_dimension()) != 1;
  };

  auto zero = b->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(output_base_shape.element_type())));
  if (ShapeSizeInBytes(lhs.base_shape()) < ShapeSizeInBytes(rhs.base_shape())) {
    if (unsupported_sharding(aligned_lhs_sharding, rhs.sharding())) {
      return nullptr;
    }
    lhs = lhs.Reshard(aligned_lhs_sharding).PadWithValue(zero);
    rhs = rhs.PadWithValue(zero);
  } else {
    if (unsupported_sharding(lhs.sharding(), aligned_rhs_sharding)) {
      return nullptr;
    }
    lhs = lhs.PadWithValue(zero);
    rhs = rhs.Reshard(aligned_rhs_sharding).PadWithValue(zero);
  }

  // Reshard RHS so that each shard computes the partial sum of the full
  // shape result, and add AllReduce. See HandleConvolutionTiledLhsAndRhs()
  // that reshards LHS.
  //
  // The size of halo on each dimension can be calculated from the
  // projection onto the RHS that shard i needs to read. RHS and LHS below
  // refers to the shard size of RHS and LHS, WC is the number of windows,
  // and D is the window dilation.
  //
  // * offset(i): LHS * i + low_padding - (WC - 1) * stride
  // * limit(i): LHS * (i + 1) + low_padding
  //
  // Since shard i has RHS of range [i * RHS * D, (i + 1) * RHS * D)
  // * left-halo: i * RHS - offset(i)
  //              = i * (RHS * D - LHS) + (WC - 1) * stride - low_padding
  // * right-halo: limit(i) - (i + 1) * RHS
  //              = (i + 1) * (LHS - RHS * D) + low_pading
  const auto& collective_ops_creator = lhs.state().collective_ops_creator;
  std::vector<int64> shard_counts(dnums.input_spatial_dimensions_size());
  std::vector<int64> lhs_shard_sizes(dnums.input_spatial_dimensions_size());
  std::vector<int64> rhs_shard_sizes(dnums.input_spatial_dimensions_size());

  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    int64 lhs_dimension = dnums.input_spatial_dimensions(i);
    int64 rhs_dimension = dnums.kernel_spatial_dimensions(i);
    int64 shard_count = rhs.sharding().tile_assignment().dim(rhs_dimension);
    const auto& wd = conv_window.dimensions(i);
    if (wd.base_dilation() != 1 || wd.window_reversal()) {
      return nullptr;
    }

    int64 lhs_shard_size =
        CeilOfRatio(lhs.base_shape().dimensions(lhs_dimension), shard_count);
    int64 rhs_shard_size =
        CeilOfRatio(rhs.base_shape().dimensions(rhs_dimension), shard_count);
    shard_counts[i] = shard_count;
    lhs_shard_sizes[i] = lhs_shard_size;
    rhs_shard_sizes[i] = rhs_shard_size;
  }

  std::vector<OffsetCalculation> left_halo_size_functions(
      output_base_shape.rank());
  std::vector<OffsetCalculation> right_halo_size_functions(
      output_base_shape.rank());
  Window new_window = conv_window;

  // Data structures needed for Pad and DynamicSlice on LHS if needed.
  bool need_dynamic_slice_lhs = false;
  auto partition_ordinals =
      MakeTiledPartitionOrdinals(lhs.sharding(), partition_id, b);
  std::vector<int64> zero_padding(output_base_shape.rank());
  PaddingConfig pad_config = window_util::MakeSymmetricPadding(zero_padding);
  auto zero_s32 =
      b->AddInstruction(HloInstruction::CreateConstant(LiteralUtil::Zero(S32)));
  std::vector<HloInstruction*> dynamic_slice_start_indices(
      output_base_shape.rank(), zero_s32);
  Shape dynamic_slice_shape = lhs.hlo()->shape();
  Shape pad_shape = lhs.hlo()->shape();

  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    int64 lhs_dimension = dnums.input_spatial_dimensions(i);
    int64 rhs_dimension = dnums.kernel_spatial_dimensions(i);
    int64 lhs_shard_size = lhs_shard_sizes[i];
    int64 rhs_shard_size = rhs_shard_sizes[i];

    if (shard_counts[i] == 1) {
      continue;
    }

    // Calculate the left and right halo sizes as described in the comments
    // above. It calculcates the halo sizes with dilation, so we apply
    // CeilOfRatio({left,right}_halo_size, window_dilation).
    const auto& wd = conv_window.dimensions(i);
    int64 padding_low = wd.padding_low();
    int64 padding_high = wd.padding_high();
    int64 base = lhs.base_shape().dimensions(lhs_dimension);
    int64 window_count = 1 + (padding_low + padding_high + base -
                              (1 + (wd.size() - 1) * wd.window_dilation())) /
                                 wd.stride();
    left_halo_size_functions[rhs_dimension] =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            rhs_shard_size * wd.window_dilation() - lhs_shard_size,
            (window_count - 1) * wd.stride() - padding_low +
                wd.window_dilation() - 1,
            wd.window_dilation()));
    right_halo_size_functions[rhs_dimension] =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            lhs_shard_size - rhs_shard_size * wd.window_dilation(),
            lhs_shard_size - rhs_shard_size * wd.window_dilation() +
                padding_low + wd.window_dilation() - 1,
            wd.window_dilation()));

    // New RHS window size includes the maximum of both left and right
    // halos.
    int64 halo_size =
        left_halo_size_functions[rhs_dimension].MaxInRange(1, shard_counts[i]) +
        right_halo_size_functions[rhs_dimension].MaxInRange(
            0, shard_counts[i] - 1);
    int64 new_window_size =
        rhs.hlo()->shape().dimensions(rhs_dimension) + halo_size;

    // The amount of new low padding could be dynamic (e.g., window_dilation
    // != 1), which requires pad (to the maximum) and dynamic slice on LHS.
    //
    // If we consider the first window, the offset of the dilated RHS that
    // aligns with the first valid LHS element for shard i is 'padding_low +
    // LHS * i'. When the left halo is added to RHS, the offset of the first
    // RHS element is (RHS * i - left_halo) * window_dilation. The
    // difference between the two values is the amount of padding_low we
    // need on LHS.
    auto new_padding_low_function =
        OffsetCalculation(HloOpcode::kMultiply,
                          left_halo_size_functions[rhs_dimension],
                          OffsetCalculation(MultiplyAddDivideOffsetCalculation(
                              0, wd.window_dilation(), 1))) -
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            rhs_shard_size * wd.window_dilation() - lhs_shard_size,
            -padding_low, 1));

    int64 new_padding_low_max =
        new_padding_low_function.MaxInRange(0, shard_counts[i]);
    int64 new_padding_low = new_padding_low_max;
    int64 new_padding_high = window_count * wd.stride() +
                             (new_window_size - 1) * wd.window_dilation() -
                             new_padding_low - lhs_shard_size;

    // We do pad/dynamic-slice only when the padding is dynamic.
    if (!new_padding_low_function.IsConstant()) {
      need_dynamic_slice_lhs = true;
      new_padding_low = 0;
      pad_config.mutable_dimensions(lhs_dimension)
          ->set_edge_padding_low(new_padding_low_max);
      pad_config.mutable_dimensions(lhs_dimension)
          ->set_edge_padding_high(new_padding_low_max);
      pad_shape.set_dimensions(lhs_dimension,
                               lhs_shard_size + 2 * new_padding_low_max);
      dynamic_slice_start_indices[lhs_dimension] =
          (OffsetCalculation(
               MultiplyAddDivideOffsetCalculation(0, new_padding_low_max, 1)) -
           new_padding_low_function)
              .Calculate(partition_ordinals[lhs_dimension], b);
      dynamic_slice_shape.set_dimensions(lhs_dimension,
                                         lhs_shard_size + new_padding_low_max);
    }

    // Since the convolution RHS operand size increased with halos, adjust
    // the window config accordingly.
    new_window.mutable_dimensions(i)->set_padding_low(new_padding_low);
    new_window.mutable_dimensions(i)->set_padding_high(new_padding_high);
    new_window.mutable_dimensions(i)->set_size(
        rhs.hlo()->shape().dimensions(rhs_dimension) + halo_size);
  }

  HloInstruction* conv_lhs = lhs.hlo();
  if (need_dynamic_slice_lhs) {
    auto pad = b->AddInstruction(
        HloInstruction::CreatePad(pad_shape, lhs.hlo(), zero, pad_config));
    conv_lhs = b->AddInstruction(HloInstruction::CreateDynamicSlice(
        dynamic_slice_shape, pad, dynamic_slice_start_indices,
        dynamic_slice_shape.dimensions()));
  }

  // Exchange halo and concatenate.
  HloInstruction* rhs_with_halo = rhs.hlo();
  for (int i = 0; i < dnums.kernel_spatial_dimensions_size(); ++i) {
    int64 dim = dnums.kernel_spatial_dimensions(i);
    int64 explicit_left_padding_on_full_shape =
        left_halo_size_functions[dim].Calculate(0);
    int64 shard_size_with_halo = new_window.dimensions(i).size();

    // offset_on_padded_shape and padded_full_shape_size are needed only if
    // we want to mask out-of-range values in ExchangeHaloAndGetValidData().
    // Since the default value for both the collective-permute is zero and
    // also we call PadWithValue() on both operands at the beginning, we
    // don't need to mask here.
    //
    // TODO(hyoulkee): Consider removing one of the two PadWithValue() calls
    // if it's always safe.
    auto offset_on_padded_shape =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            rhs_shard_sizes[i], explicit_left_padding_on_full_shape, 1)) -
        left_halo_size_functions[dim];
    int64 padded_full_shape_size =
        offset_on_padded_shape.Calculate(shard_counts[i] - 1) +
        new_window.dimensions(i).size();
    auto concat = ExchangeHaloAndGetValidData(
        rhs_with_halo, rhs.base_shape(), left_halo_size_functions[dim],
        right_halo_size_functions[dim], explicit_left_padding_on_full_shape,
        padded_full_shape_size, shard_size_with_halo, dim, rhs.sharding(),
        offset_on_padded_shape.Calculate(partition_ordinals[dim], b), zero,
        partition_ordinals[dim], collective_ops_creator,
        lhs.state().next_channel_id, b,
        /*mask_invalid_region=*/false);
    if (!concat) {
      return nullptr;
    }
    rhs_with_halo = *concat;
  }

  auto conv = b->AddInstruction(HloInstruction::CreateConvolve(
      output_base_shape, conv_lhs, rhs_with_halo,
      original_hlo->feature_group_count(), original_hlo->batch_group_count(),
      new_window, dnums, original_hlo->precision_config()));
  auto ar = collective_ops_creator.create_cross_partition_all_reduce(
      b, conv, MakeBinaryAdd(original_hlo->shape().element_type(), module), {},
      (*lhs.state().next_channel_id)++);
  ar->set_sharding(HloSharding::Replicate());
  return PartitionedHlo(ar, output_base_shape, lhs.state())
      .Reshard(output_sharding)
      .hlo();
}

// Partition convolution when both LHS and RHS are partitioned at spatial
// dimensions. Halo exchange will happen on LHS only.
StatusOr<HloInstruction*>
PartitionConvolutionWithSpatialDimensionHaloExchangeOnLHS(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, HloInstruction* partition_id,
    HloModule* module, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);
  TF_RET_CHECK(!lhs.sharding().IsTileMaximal() &&
               !rhs.sharding().IsTileMaximal());

  const auto& dnums = original_hlo->convolution_dimension_numbers();

  // Check if the operand shardings are aligned. Also we currently don't
  // support partitioning non-spatial dimensions.
  std::vector<int64> rhs_to_lhs_indices(output_base_shape.rank());
  rhs_to_lhs_indices[dnums.kernel_output_feature_dimension()] =
      dnums.input_batch_dimension();
  rhs_to_lhs_indices[dnums.kernel_input_feature_dimension()] =
      dnums.input_feature_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    rhs_to_lhs_indices[dnums.kernel_spatial_dimensions(i)] =
        dnums.input_spatial_dimensions(i);
  }
  std::vector<int64> lhs_to_rhs_indices(output_base_shape.rank());
  for (int64 i = 0; i < rhs_to_lhs_indices.size(); ++i) {
    lhs_to_rhs_indices[rhs_to_lhs_indices[i]] = i;
  }

  const Window& window = conv_window;
  std::vector<int64> reversed_rhs_dims;
  for (int64 i = 0; i < window.dimensions_size(); ++i) {
    if (window.dimensions(i).window_reversal()) {
      reversed_rhs_dims.push_back(dnums.kernel_spatial_dimensions(i));
    }
  }
  if (!reversed_rhs_dims.empty()) {
    // Make the reversed dims left-padded to prepare for window reversal.
    auto left_padded_rhs = HaloExchangeToPadOnLeft(rhs, reversed_rhs_dims);
    if (left_padded_rhs == nullptr) {
      return nullptr;
    }
    left_padded_rhs->set_sharding(rhs.sharding());
    rhs = PartitionedHlo(left_padded_rhs, rhs.base_shape(), rhs.state());
  }
  // Consider window reversal when resharding RHS or LHS. Note: this will not
  // reverse the data in the shard. We use window reversal to do that.
  auto aligned_rhs_sharding = hlo_sharding_util::ReverseSharding(
      hlo_sharding_util::TransposeSharding(lhs.sharding(), rhs_to_lhs_indices),
      reversed_rhs_dims);
  auto aligned_lhs_sharding = hlo_sharding_util::TransposeSharding(
      hlo_sharding_util::ReverseSharding(rhs.sharding(), reversed_rhs_dims),
      lhs_to_rhs_indices);

  auto unsupported_sharding = [&](const HloSharding& lhs_sharding,
                                  const HloSharding& rhs_sharding) {
    return lhs_sharding.tile_assignment().dim(dnums.input_batch_dimension()) !=
               1 ||
           rhs_sharding.tile_assignment().dim(
               dnums.kernel_output_feature_dimension()) != 1;
  };

  auto zero = b->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(output_base_shape.element_type())));
  if (ShapeSizeInBytes(lhs.base_shape()) < ShapeSizeInBytes(rhs.base_shape())) {
    if (unsupported_sharding(aligned_lhs_sharding, rhs.sharding())) {
      return nullptr;
    }
    lhs = lhs.Reshard(aligned_lhs_sharding).PadWithValue(zero);
    rhs = rhs.PadWithValue(zero, reversed_rhs_dims);
  } else {
    if (unsupported_sharding(lhs.sharding(), aligned_rhs_sharding)) {
      return nullptr;
    }
    lhs = lhs.PadWithValue(zero);
    rhs =
        rhs.Reshard(aligned_rhs_sharding).PadWithValue(zero, reversed_rhs_dims);
  }

  // Reshard LHS by exchanging halo such that each shard computes the partial
  // sum of the full shape result, and add AllReduce.
  //
  // The size of halo on each dimension can be calculated from the projection
  // onto the LHS that each RHS shard i needs to read. RHS and LHS below refers
  // to the shard size of RHS and LHS, WC is the number of windows, and D is the
  // window dilation.
  //
  // * offset(i): RHS * D * i - low_padding
  // * limit(i): {RHS * (i + 1) * D - (D - 1)} + (WC - 1) * stride - low_padding
  //
  // Since shard i has LHS of range [i * LHS, (i + 1) * LHS)
  // * left-halo: i * LHS - offset(i)
  //              = (LHS - RHS * D) * i + low_padding
  // * right-halo: limit(i) - (i + 1) * LHS
  //   = (RHS * D - LHS) * (i + 1) + (1 - D)  + (WC - 1) * stride - low_padding
  //   = (RHS * D - LHS) * i + (RHS * D - LHS) + (1-D)
  //     + (WC - 1) * stride - low_padding
  std::vector<int64> shard_counts(dnums.input_spatial_dimensions_size());
  std::vector<int64> lhs_shard_sizes(dnums.input_spatial_dimensions_size());
  std::vector<int64> rhs_shard_sizes(dnums.input_spatial_dimensions_size());
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    int64 lhs_dimension = dnums.input_spatial_dimensions(i);
    int64 rhs_dimension = dnums.kernel_spatial_dimensions(i);
    int64 shard_count = lhs.sharding().tile_assignment().dim(lhs_dimension);
    const auto& wd = window.dimensions(i);
    if (wd.base_dilation() != 1) {
      // TODO(wangtao): support parallel dim if it is replicate here.
      return nullptr;
    }

    int64 lhs_shard_size =
        CeilOfRatio(lhs.base_shape().dimensions(lhs_dimension), shard_count);
    int64 rhs_shard_size =
        CeilOfRatio(rhs.base_shape().dimensions(rhs_dimension), shard_count);
    shard_counts[i] = shard_count;
    lhs_shard_sizes[i] = lhs_shard_size;
    rhs_shard_sizes[i] = rhs_shard_size;
  }

  std::vector<OffsetCalculation> left_halo_size_functions(
      output_base_shape.rank());
  std::vector<OffsetCalculation> right_halo_size_functions(
      output_base_shape.rank());
  Window new_window = window;

  auto partition_ordinals =
      MakeTiledPartitionOrdinals(lhs.sharding(), partition_id, b);
  HloInstruction* lhs_with_halo = lhs.hlo();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    int64 lhs_dimension = dnums.input_spatial_dimensions(i);
    int64 lhs_shard_size = lhs_shard_sizes[i];
    int64 rhs_shard_size = rhs_shard_sizes[i];

    if (shard_counts[i] == 1) {
      continue;
    }

    // Calculate the left and right halo sizes as described in the comments
    // above.
    const auto& wd = window.dimensions(i);
    int64 padding_low = wd.padding_low();
    int64 padding_high = wd.padding_high();
    int64 base = lhs.base_shape().dimensions(lhs_dimension);
    int64 window_count = 1 + (padding_low + padding_high + base -
                              (1 + (wd.size() - 1) * wd.window_dilation())) /
                                 wd.stride();
    int64 rhs_shard_size_dilated =
        (rhs_shard_size - 1) * wd.window_dilation() + 1;

    left_halo_size_functions[lhs_dimension] =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            lhs_shard_size - rhs_shard_size * wd.window_dilation(), padding_low,
            1));
    right_halo_size_functions[lhs_dimension] =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation(
            rhs_shard_size * wd.window_dilation() - lhs_shard_size,
            rhs_shard_size * wd.window_dilation() - lhs_shard_size + 1 -
                wd.window_dilation() + wd.stride() * (window_count - 1) -
                padding_low,
            1));

    // Exchange halo and concatenate.
    int64 dim = dnums.input_spatial_dimensions(i);
    int64 explicit_left_padding_on_full_shape = padding_low;
    int64 shard_size_with_halo =
        wd.stride() * (window_count - 1) + rhs_shard_size_dilated;

    new_window.mutable_dimensions(i)->set_padding_low(0);
    new_window.mutable_dimensions(i)->set_padding_high(0);
    new_window.mutable_dimensions(i)->set_size(rhs_shard_size);

    // offset_on_padded_shape and padded_full_shape_size are needed only if
    // we want to mask out-of-range values in ExchangeHaloAndGetValidData().
    // Since the default value for both the collective-permute is zero and
    // also we call PadWithValue() on both operands at the beginning, we
    // don't need to mask here.
    //
    // TODO(hyoulkee): Consider removing one of the two PadWithValue() calls
    // if it's always safe.
    auto offset_on_padded_shape =
        OffsetCalculation(MultiplyAddDivideOffsetCalculation());
    int64 padded_full_shape_size = 0;
    auto concat = ExchangeHaloAndGetValidData(
        lhs_with_halo, lhs.base_shape(), left_halo_size_functions[dim],
        right_halo_size_functions[dim], explicit_left_padding_on_full_shape,
        padded_full_shape_size, shard_size_with_halo, dim, lhs.sharding(),
        offset_on_padded_shape.Calculate(partition_ordinals[dim], b), zero,
        partition_ordinals[dim], lhs.state().collective_ops_creator,
        lhs.state().next_channel_id, b,
        /*mask_invalid_region=*/false);
    if (!concat) {
      return nullptr;
    }
    lhs_with_halo = *concat;
  }

  auto conv = b->AddInstruction(HloInstruction::CreateConvolve(
      output_base_shape, lhs_with_halo, rhs.hlo(),
      original_hlo->feature_group_count(), original_hlo->batch_group_count(),
      new_window, original_hlo->convolution_dimension_numbers(),
      original_hlo->precision_config()));
  auto ar =
      lhs.state().collective_ops_creator.create_cross_partition_all_reduce(
          b, conv, MakeBinaryAdd(output_base_shape.element_type(), module), {},
          (*lhs.state().next_channel_id)++);
  ar->set_sharding(HloSharding::Replicate());
  return PartitionedHlo(ar, output_base_shape, lhs.state())
      .Reshard(output_sharding)
      .hlo();
}

// Partition convolution when output is sharded. Will shard LHS with replicated
// RHS.
StatusOr<HloInstruction*> PartitionConvolutionTiledOutput(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);
  const auto& dnums = original_hlo->convolution_dimension_numbers();
  TF_RET_CHECK(!output_sharding.IsTileMaximal());
  // We don't currently support sharding on output feature dimension.
  if (output_sharding.tile_assignment().dim(dnums.output_feature_dimension()) >
      1) {
    return nullptr;
  }

  // Check if the operand and the output sharding are aligned.
  std::vector<int64> input_to_output_indices(output_base_shape.rank());
  input_to_output_indices[dnums.input_batch_dimension()] =
      dnums.output_batch_dimension();
  input_to_output_indices[dnums.input_feature_dimension()] =
      dnums.output_feature_dimension();
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    input_to_output_indices[dnums.input_spatial_dimensions(i)] =
        dnums.output_spatial_dimensions(i);
  }
  auto target_operand_sharding = hlo_sharding_util::TransposeSharding(
      output_sharding, input_to_output_indices);
  lhs = lhs.Reshard(target_operand_sharding);

  // Replicate the RHS.
  rhs = rhs.Reshard(HloSharding::Replicate());

  // Convolution window config does not include batch and feature dimensions,
  // whereas ReshardAsWindowedInput() expects the same number of window
  // dimensions as the rank of the operand. So add two more trivial
  // dimensions.
  std::vector<int64> ones(output_base_shape.rank(), 1);
  auto operand_window = window_util::MakeWindow(ones);
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    *operand_window.mutable_dimensions(dnums.input_spatial_dimensions(i)) =
        conv_window.dimensions(i);
  }

  auto zero = b->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(output_base_shape.element_type())));
  auto resharded_operand_and_window =
      lhs.ReshardAsWindowedInput(operand_window, target_operand_sharding, zero);
  if (!resharded_operand_and_window.has_value()) {
    return nullptr;
  }
  Window new_window;
  for (int64 i = 0; i < dnums.input_spatial_dimensions_size(); ++i) {
    *new_window.add_dimensions() =
        resharded_operand_and_window->shard_window.dimensions(
            dnums.input_spatial_dimensions(i));
  }
  TF_ASSIGN_OR_RETURN(
      Shape sharded_conv_shape,
      ShapeInference::InferConvolveShape(
          resharded_operand_and_window->sharded_input->shape(),
          rhs.hlo()->shape(), original_hlo->feature_group_count(),
          original_hlo->batch_group_count(), new_window, dnums));
  auto shard_shape = MakePartitionedShape(output_base_shape, output_sharding);
  *sharded_conv_shape.mutable_layout() = shard_shape.layout();
  auto sharded_conv = b->AddInstruction(HloInstruction::CreateConvolve(
      sharded_conv_shape, resharded_operand_and_window->sharded_input,
      rhs.hlo(), original_hlo->feature_group_count(),
      original_hlo->batch_group_count(), new_window, dnums,
      original_hlo->precision_config()));
  if (!resharded_operand_and_window->dynamic_slice_index_on_output
           .has_value()) {
    CHECK(ShapeUtil::Compatible(shard_shape, sharded_conv->shape()));
    return sharded_conv;
  }
  return b->AddInstruction(HloInstruction::CreateDynamicSlice(
      shard_shape, sharded_conv,
      *resharded_operand_and_window->dynamic_slice_index_on_output,
      shard_shape.dimensions()));
}

// Partition convolution with only one kind of dims partitioned.
StatusOr<HloInstruction*> PartitionConvolutionBaseCase(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const Window& conv_window,
    HloInstruction* original_hlo, int64 num_partitions,
    const SpmdPartitionerOptions& options, HloInstruction* partition_id,
    HloModule* module, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);

  // Case 1: Handle depthwise convolution with batch group count or
  // feature group count.
  if (original_hlo->batch_group_count() > 1) {
    TF_ASSIGN_OR_RETURN(auto parallel_partitioned_conv,
                        PartitionConvolutionWithBatchGroupCount(
                            lhs, rhs, output_base_shape, output_sharding,
                            conv_window, original_hlo, num_partitions, b));
    if (parallel_partitioned_conv) {
      return parallel_partitioned_conv;
    }
  }

  if (original_hlo->feature_group_count() > 1) {
    TF_ASSIGN_OR_RETURN(auto parallel_partitioned_conv,
                        PartitionConvolutionWithFeatureGroupCount(
                            lhs, rhs, output_base_shape, output_sharding,
                            conv_window, original_hlo, num_partitions, b));
    if (parallel_partitioned_conv) {
      return parallel_partitioned_conv;
    }
  }

  // Case 2: both RHS and LHS are tiled.
  // Handling cases where both operands' shardings are aligned. We check that
  // the LHS batch dimension is not partitioned because it is mapped to the
  // output feature dimension in aligned_rhs_sharding, which are not the same
  // dimension.
  if (!lhs.sharding().IsTileMaximal() && !rhs.sharding().IsTileMaximal()) {
    if (options.conv_halo_exchange_always_on_lhs) {
      TF_ASSIGN_OR_RETURN(
          auto partitioned_conv,
          PartitionConvolutionWithSpatialDimensionHaloExchangeOnLHS(
              lhs, rhs, output_base_shape, output_sharding, conv_window,
              original_hlo, partition_id, module, b));
      if (partitioned_conv) {
        return partitioned_conv;
      }
    } else {
      TF_ASSIGN_OR_RETURN(
          auto partitioned_conv,
          PartitionConvolutionWithSpatialDimensionHaloExchangeOnRHS(
              lhs, rhs, output_base_shape, output_sharding, conv_window,
              original_hlo, partition_id, module, b));

      if (partitioned_conv) {
        return partitioned_conv;
      }
    }
  }

  // Case 3: output is tiled.
  if (!output_sharding.IsTileMaximal()) {
    TF_ASSIGN_OR_RETURN(auto partitioned_conv,
                        PartitionConvolutionTiledOutput(
                            lhs, rhs, output_base_shape, output_sharding,
                            conv_window, original_hlo, b));

    if (partitioned_conv) {
      return partitioned_conv;
    }
  }
  return nullptr;
}

}  // namespace

// Partition convolution.
StatusOr<HloInstruction*> PartitionConvolution(
    PartitionedHlo lhs, PartitionedHlo rhs, const Shape& output_base_shape,
    const HloSharding& output_sharding, const DotConvDimsMapping& dims_mapping,
    const Window& conv_window, HloInstruction* original_hlo,
    int64 num_partitions, const SpmdPartitionerOptions& options,
    HloInstruction* partition_id, HloModule* module, SpmdBuilder* b) {
  TF_RET_CHECK(original_hlo->opcode() == HloOpcode::kConvolution);

  TF_ASSIGN_OR_RETURN(
      auto try_partitioned_conv,
      PartitionConvolutionBaseCase(lhs, rhs, output_base_shape, output_sharding,
                                   conv_window, original_hlo, num_partitions,
                                   options, partition_id, module, b));
  if (try_partitioned_conv) {
    return try_partitioned_conv;
  }

  return nullptr;
}

Status SpmdPartitioningVisitor::HandleConvolution(HloInstruction* hlo) {
  auto dims_info = dot_as_convolution_util::ParseConvolutionDimsInfo(hlo);
  spmd::DotConvDimsMapping mapping;
  for (const auto& dims : dims_info.batch_dims) {
    mapping.batch_dims.emplace_back();
    mapping.batch_dims.back().lhs = dims.lhs;
    mapping.batch_dims.back().rhs = dims.rhs;
    mapping.batch_dims.back().output = dims.output;
    mapping.batch_dims.back().spatial = dims.spatial_dim;
  }
  for (const auto& dims : dims_info.contracting_dims) {
    mapping.contracting_dims.emplace_back();
    mapping.contracting_dims.back().lhs = dims.lhs;
    mapping.contracting_dims.back().rhs = dims.rhs;
    mapping.contracting_dims.back().output = dims.output;
    mapping.contracting_dims.back().spatial = dims.spatial_dim;
  }
  for (const auto& dims : dims_info.lhs_non_contracting_dims) {
    mapping.lhs_non_contracting_dims.emplace_back();
    mapping.lhs_non_contracting_dims.back().lhs = dims.lhs;
    mapping.lhs_non_contracting_dims.back().rhs = dims.rhs;
    mapping.lhs_non_contracting_dims.back().output = dims.output;
    mapping.lhs_non_contracting_dims.back().spatial = dims.spatial_dim;
  }
  for (const auto& dims : dims_info.rhs_non_contracting_dims) {
    mapping.rhs_non_contracting_dims.emplace_back();
    mapping.rhs_non_contracting_dims.back().lhs = dims.lhs;
    mapping.rhs_non_contracting_dims.back().rhs = dims.rhs;
    mapping.rhs_non_contracting_dims.back().output = dims.output;
    mapping.rhs_non_contracting_dims.back().spatial = dims.spatial_dim;
  }
  for (const auto& dims : dims_info.conv_spatial_dims) {
    mapping.conv_spatial_dims.emplace_back();
    mapping.conv_spatial_dims.back().lhs = dims.lhs;
    mapping.conv_spatial_dims.back().rhs = dims.rhs;
    mapping.conv_spatial_dims.back().output = dims.output;
    mapping.conv_spatial_dims.back().spatial = dims.spatial_dim;
  }
  auto create_sharded_conv =
      [&](HloInstruction* lhs_hlo, HloInstruction* rhs_hlo,
          spmd::SpmdBuilder* b) -> StatusOr<HloInstruction*> {
    TF_ASSIGN_OR_RETURN(
        auto sharded_conv,
        dot_as_convolution_util::CreateShardedConvForDotGeneralConvolution(
            *hlo, dims_info, lhs_hlo, rhs_hlo));
    return b->AddInstruction(std::move(sharded_conv));
  };
  return HandleDotHelper(hlo, mapping, create_sharded_conv);
}

}  // namespace spmd
}  // namespace xla
