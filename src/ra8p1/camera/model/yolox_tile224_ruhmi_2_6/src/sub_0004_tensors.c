#include "sub_0004_tensors.h"

const TensorInfo sub_0004_tensors[] = {
  { "_split_1_command_stream", 1, 24520, "COMMAND_STREAM", 0xffffffff },
  { "_split_1_flash", 2, 4625552, "MODEL", 0xffffffff },
  { "_split_1_scratch", 3, 802816, "ARENA", 0x0 },
  { "_split_1_scratch_fast", 4, 802816, "FAST_SCRATCH", 0x0 },
  { "model_3_tf_strided_slice_StridedSlice1_70183", 8, 37632, "INPUT_TENSOR", 0x1b900 },
  { "model_3_tf_strided_slice_2_StridedSlice1_70181", 6, 37632, "INPUT_TENSOR", 0x12600 },
  { "model_3_tf_strided_slice_1_StridedSlice3_70180", 5, 37632, "INPUT_TENSOR", 0x9300 },
  { "model_3_tf_strided_slice_3_StridedSlice1_70182", 7, 37632, "INPUT_TENSOR", 0x0 },
  { "PartitionedCall_0_70473", 0, 6174, "OUTPUT_TENSOR", 0x0 },
};

const size_t sub_0004_tensors_count = sizeof(sub_0004_tensors) / sizeof(sub_0004_tensors[0]);

// Addresses for each input and output buffer inside of the arena
const uint32_t sub_0004_address_model_3_tf_strided_slice_StridedSlice1_70183 = 0x1b900;
const uint32_t sub_0004_address_model_3_tf_strided_slice_2_StridedSlice1_70181 = 0x12600;
const uint32_t sub_0004_address_model_3_tf_strided_slice_1_StridedSlice3_70180 = 0x9300;
const uint32_t sub_0004_address_model_3_tf_strided_slice_3_StridedSlice1_70182 = 0x0;
const uint32_t sub_0004_address_PartitionedCall_0_70473 = 0x0;

