"""Deterministic ONNX ABI transforms for RUHMI deployment candidates."""

from __future__ import annotations

from collections import defaultdict
import hashlib
from pathlib import Path
from typing import Any

import numpy as np

from .contract import sha256_file
from .postprocess import build_yolov8_anchor_grid


TRANSFORM_ID = "split-terminal-concat-v1"
BOXES_OUTPUT_NAME = "boxes"
SCORES_OUTPUT_NAME = "scores"
SOURCE_OUTPUT_SHAPE = [1, 5, 8400]
BOXES_OUTPUT_SHAPE = [1, 4, 8400]
SCORES_OUTPUT_SHAPE = [1, 1, 8400]
FULL320_SOURCE_OUTPUT_SHAPE = [1, 5, 2100]
FULL320_BOXES_OUTPUT_SHAPE = [1, 4, 2100]
FULL320_SCORES_OUTPUT_SHAPE = [1, 1, 2100]
_SPLIT_OUTPUT_PROFILES = (
    {
        "model_size": 320,
        "candidate_count": 2100,
        "input_shape": [1, 3, 320, 320],
        "source_output_shape": FULL320_SOURCE_OUTPUT_SHAPE,
        "boxes_output_shape": FULL320_BOXES_OUTPUT_SHAPE,
        "scores_output_shape": FULL320_SCORES_OUTPUT_SHAPE,
    },
    {
        "model_size": 640,
        "candidate_count": 8400,
        "input_shape": [1, 3, 640, 640],
        "source_output_shape": SOURCE_OUTPUT_SHAPE,
        "boxes_output_shape": BOXES_OUTPUT_SHAPE,
        "scores_output_shape": SCORES_OUTPUT_SHAPE,
    },
)
PREDECODE_TRANSFORM_ID = "remove-yolov8-detect-tail-v1"
PREDECODE_IDENTITY_WRAPPER_TRANSFORM_ID = "wrap-predecode-output-identities-v1"
REGRESSION_LOGITS_OUTPUT_NAME = "regression_logits"
CLASS_LOGITS_OUTPUT_NAME = "class_logits"
REGRESSION_LOGITS_OUTPUT_SHAPE = [1, 64, 8400]
CLASS_LOGITS_OUTPUT_SHAPE = [1, 1, 8400]
QAT_FULL320_DFL_PROJECTION = np.asarray(
    [
        0.0,
        0.94482421875,
        2.0078125,
        2.953125,
        4.015625,
        4.9609375,
        6.0234375,
        6.96875,
        8.03125,
        8.9765625,
        10.0390625,
        10.984375,
        12.046875,
        12.9921875,
        14.0546875,
        15.0,
    ],
    dtype=np.float32,
)

_PREDECODE_SOURCE_TENSORS = (
    "/model.22/Split_output_0",
    "/model.22/Split_output_1",
)
_PREDECODE_TAIL_NODE_SPECS = {
    "/model.22/Add": (
        "Add",
        ["/model.22/Gather_output_0", "/model.22/Constant_5_output_0"],
        ["/model.22/Add_output_0"],
    ),
    "/model.22/Div": (
        "Div",
        ["/model.22/Add_output_0", "/model.22/Constant_6_output_0"],
        ["/model.22/Mul_output_0"],
    ),
    "/model.22/Mul_1": (
        "Mul",
        ["/model.22/Mul_output_0", "/model.22/Constant_6_output_0"],
        ["/model.22/Mul_1_output_0"],
    ),
    "/model.22/dfl/Reshape": (
        "Reshape",
        [
            "/model.22/Split_output_0",
            "/model.22/dfl/Constant_output_0",
        ],
        ["/model.22/dfl/Reshape_output_0"],
    ),
    "/model.22/Sigmoid": (
        "Sigmoid",
        ["/model.22/Split_output_1"],
        ["/model.22/Sigmoid_output_0"],
    ),
    "/model.22/dfl/Transpose": (
        "Transpose",
        ["/model.22/dfl/Reshape_output_0"],
        ["/model.22/dfl/Transpose_output_0"],
    ),
    "/model.22/dfl/Softmax": (
        "Softmax",
        ["/model.22/dfl/Transpose_output_0"],
        ["/model.22/dfl/Softmax_output_0"],
    ),
    "/model.22/dfl/Transpose_1": (
        "Transpose",
        ["/model.22/dfl/Softmax_output_0"],
        ["/model.22/dfl/Transpose_1_output_0"],
    ),
    "/model.22/dfl/conv/Conv": (
        "Conv",
        [
            "/model.22/dfl/Transpose_1_output_0",
            "model.22.dfl.conv.weight",
        ],
        ["/model.22/dfl/conv/Conv_output_0"],
    ),
    "/model.22/dfl/Reshape_1": (
        "Reshape",
        [
            "/model.22/dfl/conv/Conv_output_0",
            "/model.22/dfl/Constant_1_output_0",
        ],
        ["/model.22/dfl/Reshape_1_output_0"],
    ),
    "/model.22/Slice": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Constant_4_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Constant_5_output_0",
        ],
        ["/model.22/Slice_output_0"],
    ),
    "/model.22/Slice_1": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Mul_1_output_0",
            "/model.22/Constant_5_output_0",
        ],
        ["/model.22/Slice_1_output_0"],
    ),
    "/model.22/Sub": (
        "Sub",
        ["/model.22/Constant_9_output_0", "/model.22/Slice_output_0"],
        ["/model.22/Sub_output_0"],
    ),
    "/model.22/Add_1": (
        "Add",
        ["/model.22/Constant_10_output_0", "/model.22/Slice_1_output_0"],
        ["/model.22/Add_1_output_0"],
    ),
    "/model.22/Add_2": (
        "Add",
        ["/model.22/Sub_output_0", "/model.22/Add_1_output_0"],
        ["/model.22/Add_2_output_0"],
    ),
    "/model.22/Sub_1": (
        "Sub",
        ["/model.22/Add_1_output_0", "/model.22/Sub_output_0"],
        ["/model.22/Sub_1_output_0"],
    ),
    "/model.22/Div_1": (
        "Div",
        ["/model.22/Add_2_output_0", "/model.22/Constant_11_output_0"],
        ["/model.22/Div_1_output_0"],
    ),
    "/model.22/Concat_4": (
        "Concat",
        ["/model.22/Div_1_output_0", "/model.22/Sub_1_output_0"],
        ["/model.22/Concat_4_output_0"],
    ),
    "/model.22/Mul_2": (
        "Mul",
        ["/model.22/Concat_4_output_0", "/model.22/Constant_12_output_0"],
        ["/model.22/Mul_2_output_0"],
    ),
    "/model.22/Concat_5": (
        "Concat",
        ["/model.22/Mul_2_output_0", "/model.22/Sigmoid_output_0"],
        ["output0"],
    ),
}

_PREDECODE_TAIL_ATTRIBUTE_SPECS = {
    "/model.22/dfl/Transpose": {"perm": [0, 3, 1, 2]},
    "/model.22/dfl/Softmax": {"axis": 3},
    "/model.22/dfl/Transpose_1": {"perm": [0, 3, 2, 1]},
    "/model.22/dfl/conv/Conv": {
        "dilations": [1, 1],
        "group": 1,
        "kernel_shape": [1, 1],
        "pads": [0, 0, 0, 0],
        "strides": [1, 1],
    },
    "/model.22/Concat_4": {"axis": 1},
    "/model.22/Concat_5": {"axis": 1},
}

_PREDECODE_UNUSED_INITIALIZERS = (
    "/model.22/Gather_output_0",
    "/model.22/Constant_5_output_0",
    "/model.22/Constant_6_output_0",
    "/model.22/dfl/Constant_output_0",
    "model.22.dfl.conv.weight",
    "/model.22/dfl/Constant_1_output_0",
    "/model.22/Constant_4_output_0",
    "/model.22/Constant_9_output_0",
    "/model.22/Constant_10_output_0",
    "/model.22/Constant_11_output_0",
    "/model.22/Constant_12_output_0",
)

_PREDECODE_TAIL_NODE_SPECS_320 = {
    "/model.22/dfl/Reshape": (
        "Reshape",
        [
            "/model.22/Split_output_0",
            "/model.22/dfl/Constant_output_0",
        ],
        ["/model.22/dfl/Reshape_output_0"],
    ),
    "/model.22/Sigmoid": (
        "Sigmoid",
        ["/model.22/Split_output_1"],
        ["/model.22/Sigmoid_output_0"],
    ),
    "/model.22/dfl/Transpose": (
        "Transpose",
        ["/model.22/dfl/Reshape_output_0"],
        ["/model.22/dfl/Transpose_output_0"],
    ),
    "/model.22/dfl/Softmax": (
        "Softmax",
        ["/model.22/dfl/Transpose_output_0"],
        ["/model.22/dfl/Softmax_output_0"],
    ),
    "/model.22/dfl/Transpose_1": (
        "Transpose",
        ["/model.22/dfl/Softmax_output_0"],
        ["/model.22/dfl/Transpose_1_output_0"],
    ),
    "/model.22/dfl/conv/Conv": (
        "Conv",
        [
            "/model.22/dfl/Transpose_1_output_0",
            "model.22.dfl.conv.weight",
        ],
        ["/model.22/dfl/conv/Conv_output_0"],
    ),
    "/model.22/dfl/Reshape_1": (
        "Reshape",
        [
            "/model.22/dfl/conv/Conv_output_0",
            "/model.22/dfl/Constant_1_output_0",
        ],
        ["/model.22/dfl/Reshape_1_output_0"],
    ),
    "/model.22/Slice": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Constant_4_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Constant_3_output_0",
        ],
        ["/model.22/Slice_output_0"],
    ),
    "/model.22/Slice_1": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Mul_1_output_0",
            "/model.22/Constant_3_output_0",
        ],
        ["/model.22/Slice_1_output_0"],
    ),
    "/model.22/Sub": (
        "Sub",
        ["/model.22/Constant_9_output_0", "/model.22/Slice_output_0"],
        ["/model.22/Sub_output_0"],
    ),
    "/model.22/Add_1": (
        "Add",
        ["/model.22/Constant_10_output_0", "/model.22/Slice_1_output_0"],
        ["/model.22/Add_1_output_0"],
    ),
    "/model.22/Add_2": (
        "Add",
        ["/model.22/Sub_output_0", "/model.22/Add_1_output_0"],
        ["/model.22/Add_2_output_0"],
    ),
    "/model.22/Sub_1": (
        "Sub",
        ["/model.22/Add_1_output_0", "/model.22/Sub_output_0"],
        ["/model.22/Sub_1_output_0"],
    ),
    "/model.22/Div_1": (
        "Div",
        ["/model.22/Add_2_output_0", "/model.22/Constant_11_output_0"],
        ["/model.22/Div_1_output_0"],
    ),
    "/model.22/Concat_4": (
        "Concat",
        ["/model.22/Div_1_output_0", "/model.22/Sub_1_output_0"],
        ["/model.22/Concat_4_output_0"],
    ),
    "/model.22/Mul_2": (
        "Mul",
        ["/model.22/Concat_4_output_0", "/model.22/Constant_12_output_0"],
        ["/model.22/Mul_2_output_0"],
    ),
    "/model.22/Concat_5": (
        "Concat",
        ["/model.22/Mul_2_output_0", "/model.22/Sigmoid_output_0"],
        ["output0"],
    ),
}

_PREDECODE_UNUSED_INITIALIZERS_320 = (
    "/model.22/dfl/Constant_output_0",
    "model.22.dfl.conv.weight",
    "/model.22/dfl/Constant_1_output_0",
    "/model.22/Constant_4_output_0",
    "/model.22/Mul_output_0",
    "/model.22/Constant_3_output_0",
    "/model.22/Mul_1_output_0",
    "/model.22/Constant_9_output_0",
    "/model.22/Constant_10_output_0",
    "/model.22/Constant_11_output_0",
    "/model.22/Constant_12_output_0",
)

_PREDECODE_TAIL_NODE_SPECS_320_OPSET13 = {
    "/model.22/dfl/Constant": (
        "Constant",
        [],
        ["/model.22/dfl/Constant_output_0"],
    ),
    "/model.22/dfl/Reshape": (
        "Reshape",
        ["/model.22/Split_output_0", "/model.22/dfl/Constant_output_0"],
        ["/model.22/dfl/Reshape_output_0"],
    ),
    "/model.22/dfl/Transpose": (
        "Transpose",
        ["/model.22/dfl/Reshape_output_0"],
        ["/model.22/dfl/Transpose_output_0"],
    ),
    "/model.22/dfl/Softmax": (
        "Softmax",
        ["/model.22/dfl/Transpose_output_0"],
        ["/model.22/dfl/Softmax_output_0"],
    ),
    "/model.22/dfl/conv/Conv": (
        "Conv",
        ["/model.22/dfl/Softmax_output_0", "model.22.dfl.conv.weight"],
        ["/model.22/dfl/conv/Conv_output_0"],
    ),
    "/model.22/dfl/Constant_1": (
        "Constant",
        [],
        ["/model.22/dfl/Constant_1_output_0"],
    ),
    "/model.22/dfl/Reshape_1": (
        "Reshape",
        ["/model.22/dfl/conv/Conv_output_0", "/model.22/dfl/Constant_1_output_0"],
        ["/model.22/dfl/Reshape_1_output_0"],
    ),
    "/model.22/Shape": (
        "Shape",
        ["/model.22/dfl/Reshape_1_output_0"],
        ["/model.22/Shape_output_0"],
    ),
    "/model.22/Constant_3": (
        "Constant",
        [],
        ["/model.22/Constant_3_output_0"],
    ),
    "/model.22/Gather": (
        "Gather",
        ["/model.22/Shape_output_0", "/model.22/Constant_3_output_0"],
        ["/model.22/Gather_output_0"],
    ),
    "/model.22/Constant_4": (
        "Constant",
        [],
        ["/model.22/Constant_4_output_0"],
    ),
    "/model.22/Constant_5": (
        "Constant",
        [],
        ["/model.22/Constant_5_output_0"],
    ),
    "/model.22/Add": (
        "Add",
        ["/model.22/Gather_output_0", "/model.22/Constant_5_output_0"],
        ["/model.22/Add_output_0"],
    ),
    "/model.22/Constant_6": (
        "Constant",
        [],
        ["/model.22/Constant_6_output_0"],
    ),
    "/model.22/Div": (
        "Div",
        ["/model.22/Add_output_0", "/model.22/Constant_6_output_0"],
        ["/model.22/Div_output_0"],
    ),
    "/model.22/Constant_7": (
        "Constant",
        [],
        ["/model.22/Constant_7_output_0"],
    ),
    "/model.22/Mul": (
        "Mul",
        ["/model.22/Div_output_0", "/model.22/Constant_7_output_0"],
        ["/model.22/Mul_output_0"],
    ),
    "/model.22/Slice": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Constant_4_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Constant_3_output_0",
        ],
        ["/model.22/Slice_output_0"],
    ),
    "/model.22/Constant_8": (
        "Constant",
        [],
        ["/model.22/Constant_8_output_0"],
    ),
    "/model.22/Mul_1": (
        "Mul",
        ["/model.22/Div_output_0", "/model.22/Constant_8_output_0"],
        ["/model.22/Mul_1_output_0"],
    ),
    "/model.22/Slice_1": (
        "Slice",
        [
            "/model.22/dfl/Reshape_1_output_0",
            "/model.22/Mul_output_0",
            "/model.22/Mul_1_output_0",
            "/model.22/Constant_3_output_0",
        ],
        ["/model.22/Slice_1_output_0"],
    ),
    "/model.22/Constant_9": (
        "Constant",
        [],
        ["/model.22/Constant_9_output_0"],
    ),
    "/model.22/Sub": (
        "Sub",
        ["/model.22/Constant_9_output_0", "/model.22/Slice_output_0"],
        ["/model.22/Sub_output_0"],
    ),
    "/model.22/Constant_10": (
        "Constant",
        [],
        ["/model.22/Constant_10_output_0"],
    ),
    "/model.22/Add_1": (
        "Add",
        ["/model.22/Constant_10_output_0", "/model.22/Slice_1_output_0"],
        ["/model.22/Add_1_output_0"],
    ),
    "/model.22/Add_2": (
        "Add",
        ["/model.22/Sub_output_0", "/model.22/Add_1_output_0"],
        ["/model.22/Add_2_output_0"],
    ),
    "/model.22/Constant_11": (
        "Constant",
        [],
        ["/model.22/Constant_11_output_0"],
    ),
    "/model.22/Div_1": (
        "Div",
        ["/model.22/Add_2_output_0", "/model.22/Constant_11_output_0"],
        ["/model.22/Div_1_output_0"],
    ),
    "/model.22/Sub_1": (
        "Sub",
        ["/model.22/Add_1_output_0", "/model.22/Sub_output_0"],
        ["/model.22/Sub_1_output_0"],
    ),
    "/model.22/Concat_4": (
        "Concat",
        ["/model.22/Div_1_output_0", "/model.22/Sub_1_output_0"],
        ["/model.22/Concat_4_output_0"],
    ),
    "/model.22/Constant_12": (
        "Constant",
        [],
        ["/model.22/Constant_12_output_0"],
    ),
    "/model.22/Mul_2": (
        "Mul",
        ["/model.22/Concat_4_output_0", "/model.22/Constant_12_output_0"],
        ["/model.22/Mul_2_output_0"],
    ),
    "/model.22/Sigmoid": (
        "Sigmoid",
        ["/model.22/Split_output_1"],
        ["/model.22/Sigmoid_output_0"],
    ),
    "/model.22/Concat_5": (
        "Concat",
        ["/model.22/Mul_2_output_0", "/model.22/Sigmoid_output_0"],
        ["output0"],
    ),
}

_PREDECODE_TAIL_ATTRIBUTE_SPECS_320_OPSET13 = {
    "/model.22/dfl/Transpose": {"perm": [0, 2, 1, 3]},
    "/model.22/dfl/Softmax": {"axis": 1},
    "/model.22/dfl/conv/Conv": {
        "dilations": [1, 1],
        "group": 1,
        "kernel_shape": [1, 1],
        "pads": [0, 0, 0, 0],
        "strides": [1, 1],
    },
    "/model.22/Gather": {"axis": 0},
    "/model.22/Concat_4": {"axis": 1},
    "/model.22/Concat_5": {"axis": 1},
}

_PREDECODE_UNUSED_INITIALIZERS_320_OPSET13 = (
    "model.22.dfl.conv.weight",
)


def wrap_predecode_output_identities(
    source_path: Path,
    output_path: Path,
) -> dict[str, Any]:
    """Give each predecode graph output a unique Identity producer for MERA."""

    if source_path.resolve() == output_path.resolve():
        raise ValueError("source and wrapped output paths must differ")
    try:
        import onnx
    except ImportError as exc:
        raise RuntimeError("onnx is required for model derivation") from exc

    source_sha256 = sha256_file(source_path)
    model = onnx.load(str(source_path), load_external_data=False)
    onnx.checker.check_model(model)
    inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    metadata = {item.key: item.value for item in model.metadata_props}
    if metadata.get("renesas_cup.transform") != PREDECODE_TRANSFORM_ID:
        raise ValueError("identity wrapper input must be a predecode-output model")
    observed_outputs = [_tensor_contract(item) for item in inferred.graph.output]
    if len(observed_outputs) != 2:
        raise ValueError("predecode identity wrapper requires exactly two outputs")
    candidate_count = observed_outputs[0]["shape"][-1]
    expected_outputs = [
        {
            "name": REGRESSION_LOGITS_OUTPUT_NAME,
            "dtype": "float32",
            "shape": [1, 64, candidate_count],
        },
        {
            "name": CLASS_LOGITS_OUTPUT_NAME,
            "dtype": "float32",
            "shape": [1, 1, candidate_count],
        },
    ]
    if candidate_count not in (2100, 8400) or observed_outputs != expected_outputs:
        raise ValueError(
            f"unsupported predecode output contract: {observed_outputs}"
        )

    internal_names = {
        REGRESSION_LOGITS_OUTPUT_NAME: "regression_logits_internal",
        CLASS_LOGITS_OUTPUT_NAME: "class_logits_internal",
    }
    _require_available_output_names(model, set(), set(internal_names.values()))
    for output_name, internal_name in internal_names.items():
        _rename_tensor(model, output_name, internal_name)
        model.graph.node.append(
            onnx.helper.make_node(
                "Identity",
                [internal_name],
                [output_name],
                name=f"renesas_cup/{output_name}_output_identity",
            )
        )

    metadata.update(
        {
            "renesas_cup.transform.identity_wrapper": (
                PREDECODE_IDENTITY_WRAPPER_TRANSFORM_ID
            ),
            "renesas_cup.transform.identity_wrapper.source_sha256": source_sha256,
        }
    )
    onnx.helper.set_model_props(model, metadata)
    onnx.checker.check_model(model)
    wrapped_inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    wrapped_outputs = [
        _tensor_contract(item) for item in wrapped_inferred.graph.output
    ]
    if wrapped_outputs != expected_outputs:
        raise ValueError(f"wrapped output contract mismatch: {wrapped_outputs}")
    output_producers = {
        output.name: [node for node in model.graph.node if output.name in node.output]
        for output in model.graph.output
    }
    if any(
        len(nodes) != 1 or nodes[0].op_type != "Identity"
        for nodes in output_producers.values()
    ):
        raise ValueError("each wrapped output must have one unique Identity producer")

    serialized = model.SerializeToString(deterministic=True)
    wrapped_sha256 = hashlib.sha256(serialized).hexdigest()
    if output_path.exists():
        if output_path.read_bytes() != serialized:
            raise FileExistsError(
                f"refusing to replace non-matching wrapped model: {output_path}"
            )
    else:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(serialized)
    return {
        "transform_id": PREDECODE_IDENTITY_WRAPPER_TRANSFORM_ID,
        "source_bytes": source_path.stat().st_size,
        "source_sha256": source_sha256,
        "wrapped_bytes": len(serialized),
        "wrapped_sha256": wrapped_sha256,
        "candidate_count": candidate_count,
        "outputs": expected_outputs,
    }


def derive_predecode_output_model(
    source_path: Path,
    output_path: Path,
) -> dict[str, Any]:
    """Expose YOLOv8 regression/class logits after removing the decode tail."""

    if source_path.resolve() == output_path.resolve():
        raise ValueError("source and derived output paths must differ")
    try:
        import onnx
    except ImportError as exc:
        raise RuntimeError("onnx is required for model derivation") from exc

    source_sha256 = sha256_file(source_path)
    model = onnx.load(str(source_path), load_external_data=False)
    onnx.checker.check_model(model)
    try:
        inferred_model = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    except Exception as exc:
        raise ValueError(f"source shape inference failed: {exc}") from exc
    profile = _select_predecode_profile(model.graph, inferred_model)
    topology = _validate_predecode_tail(model, inferred_model, onnx, profile)

    _require_available_output_names(
        model,
        set(_PREDECODE_SOURCE_TENSORS),
        {REGRESSION_LOGITS_OUTPUT_NAME, CLASS_LOGITS_OUTPUT_NAME},
    )
    retained_nodes = [
        node
        for node_index, node in enumerate(model.graph.node)
        if node_index not in topology["tail_node_indexes"]
    ]
    del model.graph.node[:]
    model.graph.node.extend(retained_nodes)

    removed_initializer_names = set(topology["unused_initializer_names"])
    retained_initializers = [
        initializer
        for initializer in model.graph.initializer
        if initializer.name not in removed_initializer_names
    ]
    del model.graph.initializer[:]
    model.graph.initializer.extend(retained_initializers)

    _rename_tensor(
        model,
        _PREDECODE_SOURCE_TENSORS[0],
        REGRESSION_LOGITS_OUTPUT_NAME,
    )
    _rename_tensor(
        model,
        _PREDECODE_SOURCE_TENSORS[1],
        CLASS_LOGITS_OUTPUT_NAME,
    )
    del model.graph.output[:]
    model.graph.output.extend(
        [
            onnx.helper.make_tensor_value_info(
                REGRESSION_LOGITS_OUTPUT_NAME,
                onnx.TensorProto.FLOAT,
                profile["regression_output_shape"],
            ),
            onnx.helper.make_tensor_value_info(
                CLASS_LOGITS_OUTPUT_NAME,
                onnx.TensorProto.FLOAT,
                profile["class_output_shape"],
            ),
        ]
    )
    _prune_stale_value_info(model)

    metadata = {item.key: item.value for item in model.metadata_props}
    metadata.update(
        {
            "renesas_cup.transform": PREDECODE_TRANSFORM_ID,
            "renesas_cup.transform.output_assembly": (
                "reconstruct_yolov8_output(regression_logits, class_logits)"
            ),
            "renesas_cup.transform.removed_initializers": ",".join(
                topology["unused_initializer_names"]
            ),
            "renesas_cup.transform.removed_nodes": ",".join(
                topology["tail_node_names"]
            ),
            "renesas_cup.transform.split_encoding": topology["split_encoding"],
            "renesas_cup.transform.source_sha256": source_sha256,
        }
    )
    onnx.helper.set_model_props(model, metadata)
    onnx.checker.check_model(model)
    derived_inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    observed_outputs = [_tensor_contract(item) for item in derived_inferred.graph.output]
    expected_outputs = [
        {
            "name": REGRESSION_LOGITS_OUTPUT_NAME,
            "dtype": "float32",
            "shape": profile["regression_output_shape"],
        },
        {
            "name": CLASS_LOGITS_OUTPUT_NAME,
            "dtype": "float32",
            "shape": profile["class_output_shape"],
        },
    ]
    if observed_outputs != expected_outputs:
        raise ValueError(f"derived output contract mismatch: {observed_outputs}")

    serialized = model.SerializeToString(deterministic=True)
    derived_sha256 = hashlib.sha256(serialized).hexdigest()
    if output_path.exists():
        if output_path.read_bytes() != serialized:
            raise FileExistsError(
                f"refusing to replace non-matching derived model: {output_path}"
            )
    else:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(serialized)

    return {
        "transform_id": PREDECODE_TRANSFORM_ID,
        "model_size": profile["model_size"],
        "candidate_count": profile["candidate_count"],
        "source_bytes": source_path.stat().st_size,
        "source_sha256": source_sha256,
        "derived_bytes": len(serialized),
        "derived_sha256": derived_sha256,
        "removed_node_names": topology["tail_node_names"],
        "removed_initializer_names": topology["unused_initializer_names"],
        "anchor_grid_sha256": topology["anchor_grid_sha256"],
        "stride_sha256": topology["stride_sha256"],
        "dfl_weight_sha256": topology["dfl_weight_sha256"],
        "split_encoding": topology["split_encoding"],
        "outputs": expected_outputs,
    }


def derive_split_output_model(source_path: Path, output_path: Path) -> dict[str, Any]:
    """Remove only the terminal YOLO boxes/scores Concat and expose both inputs."""

    if source_path.resolve() == output_path.resolve():
        raise ValueError("source and derived output paths must differ")
    try:
        import onnx
    except ImportError as exc:
        raise RuntimeError("onnx is required for model derivation") from exc

    source_sha256 = sha256_file(source_path)
    model = onnx.load(str(source_path), load_external_data=False)
    onnx.checker.check_model(model)
    inferred_model = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    topology = _validate_terminal_concat(model, inferred_model, onnx)

    final_node_index = topology["final_node_index"]
    boxes_source_name = topology["boxes_source_name"]
    scores_source_name = topology["scores_source_name"]
    boxes_output_shape = topology["boxes_output_shape"]
    scores_output_shape = topology["scores_output_shape"]
    _require_available_output_names(
        model,
        {boxes_source_name, scores_source_name},
        {BOXES_OUTPUT_NAME, SCORES_OUTPUT_NAME},
    )

    del model.graph.node[final_node_index]
    _rename_tensor(model, boxes_source_name, BOXES_OUTPUT_NAME)
    _rename_tensor(model, scores_source_name, SCORES_OUTPUT_NAME)
    del model.graph.output[:]
    model.graph.output.extend(
        [
            onnx.helper.make_tensor_value_info(
                BOXES_OUTPUT_NAME,
                onnx.TensorProto.FLOAT,
                boxes_output_shape,
            ),
            onnx.helper.make_tensor_value_info(
                SCORES_OUTPUT_NAME,
                onnx.TensorProto.FLOAT,
                scores_output_shape,
            ),
        ]
    )

    metadata = {item.key: item.value for item in model.metadata_props}
    metadata.update(
        {
            "renesas_cup.transform": TRANSFORM_ID,
            "renesas_cup.transform.output_assembly": (
                "np.concatenate([boxes, scores], axis=1)"
            ),
            "renesas_cup.transform.removed_node": topology["removed_node_name"],
            "renesas_cup.transform.source_sha256": source_sha256,
        }
    )
    onnx.helper.set_model_props(model, metadata)
    onnx.checker.check_model(model)
    derived_inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    observed_outputs = [_tensor_contract(item) for item in derived_inferred.graph.output]
    expected_outputs = [
        {"name": BOXES_OUTPUT_NAME, "dtype": "float32", "shape": boxes_output_shape},
        {
            "name": SCORES_OUTPUT_NAME,
            "dtype": "float32",
            "shape": scores_output_shape,
        },
    ]
    if observed_outputs != expected_outputs:
        raise ValueError(f"derived output contract mismatch: {observed_outputs}")

    serialized = model.SerializeToString(deterministic=True)
    derived_sha256 = hashlib.sha256(serialized).hexdigest()
    if output_path.exists():
        if output_path.read_bytes() != serialized:
            raise FileExistsError(
                f"refusing to replace non-matching derived model: {output_path}"
            )
    else:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(serialized)

    return {
        "transform_id": TRANSFORM_ID,
        "model_size": topology["model_size"],
        "candidate_count": topology["candidate_count"],
        "source_bytes": source_path.stat().st_size,
        "source_sha256": source_sha256,
        "derived_bytes": len(serialized),
        "derived_sha256": derived_sha256,
        "removed_node_name": topology["removed_node_name"],
        "outputs": expected_outputs,
    }


def _select_predecode_profile(graph: Any, inferred_model: Any) -> dict[str, Any]:
    if len(graph.input) != 1 or len(graph.output) != 1:
        raise ValueError("predecode source must have exactly one input and one output")
    input_contract = _tensor_contract(
        _find_value_info(inferred_model, graph.input[0].name)
    )
    output_contract = _tensor_contract(inferred_model.graph.output[0])
    profile_shapes = {
        (320, 2100): ([1, 3, 320, 320], [1, 5, 2100]),
        (640, 8400): ([1, 3, 640, 640], [1, 5, 8400]),
    }
    for (model_size, candidate_count), (input_shape, output_shape) in profile_shapes.items():
        if (
            input_contract
            == {"name": "images", "dtype": "float32", "shape": input_shape}
            and output_contract["dtype"] == "float32"
            and output_contract["shape"] == output_shape
        ):
            is_320 = model_size == 320
            split_nodes = [node for node in graph.node if node.name == "/model.22/Split"]
            uses_opset13_constants = (
                is_320
                and len(split_nodes) == 1
                and list(split_nodes[0].input)
                == ["/model.22/Concat_3_output_0", "onnx::Split_388"]
            )
            return {
                "model_size": model_size,
                "candidate_count": candidate_count,
                "input_shape": input_shape,
                "source_output_shape": output_shape,
                "regression_output_shape": [1, 64, candidate_count],
                "class_output_shape": [1, 1, candidate_count],
                "tail_node_specs": (
                    _PREDECODE_TAIL_NODE_SPECS_320_OPSET13
                    if uses_opset13_constants
                    else (
                        _PREDECODE_TAIL_NODE_SPECS_320
                        if is_320
                        else _PREDECODE_TAIL_NODE_SPECS
                    )
                ),
                "tail_attribute_specs": (
                    _PREDECODE_TAIL_ATTRIBUTE_SPECS_320_OPSET13
                    if uses_opset13_constants
                    else _PREDECODE_TAIL_ATTRIBUTE_SPECS
                ),
                "unused_initializers": (
                    _PREDECODE_UNUSED_INITIALIZERS_320_OPSET13
                    if uses_opset13_constants
                    else (
                        _PREDECODE_UNUSED_INITIALIZERS_320
                        if is_320
                        else _PREDECODE_UNUSED_INITIALIZERS
                    )
                ),
                "split_encoding": (
                    "opset13-static-input"
                    if uses_opset13_constants
                    else "split-attribute"
                ),
                "constants_in_nodes": uses_opset13_constants,
            }
    raise ValueError(
        "predecode transform supports only fixed FP32 320/2100 or 640/8400 "
        f"contracts: input={input_contract} output={output_contract}"
    )


def _validate_predecode_tail(
    model: Any,
    inferred_model: Any,
    onnx: Any,
    profile: dict[str, Any],
) -> dict[str, Any]:
    graph = model.graph
    _validate_source_contract(
        graph,
        inferred_model,
        input_shape=profile["input_shape"],
        output_shape=profile["source_output_shape"],
    )

    nodes_by_name: dict[str, list[tuple[int, Any]]] = defaultdict(list)
    producers: dict[str, list[tuple[int, Any]]] = defaultdict(list)
    consumers: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for node_index, node in enumerate(graph.node):
        nodes_by_name[node.name].append((node_index, node))
        for output_name in node.output:
            producers[output_name].append((node_index, node))
        for input_index, input_name in enumerate(node.input):
            consumers[input_name].append((node_index, input_index))

    split_matches = nodes_by_name["/model.22/Split"]
    if len(split_matches) != 1:
        raise ValueError(
            "expected exactly one /model.22/Split node, "
            f"found {len(split_matches)}"
        )
    split_index, split_node = split_matches[0]
    expected_split_inputs = ["/model.22/Concat_3_output_0"]
    if profile["split_encoding"] == "opset13-static-input":
        expected_split_inputs.append("onnx::Split_388")
    expected_split_spec = (
        "Split",
        expected_split_inputs,
        list(_PREDECODE_SOURCE_TENSORS),
    )
    observed_split_spec = (
        split_node.op_type,
        list(split_node.input),
        list(split_node.output),
    )
    if observed_split_spec != expected_split_spec or split_node.domain:
        raise ValueError(
            f"/model.22/Split topology mismatch: {observed_split_spec}"
        )
    split_attributes = _node_attributes(split_node, onnx)
    expected_split_attributes = {"axis": 1}
    if profile["split_encoding"] == "split-attribute":
        expected_split_attributes["split"] = [64, 1]
    if split_attributes != expected_split_attributes:
        raise ValueError(
            "/model.22/Split attributes do not match the frozen static split, "
            f"got {split_attributes}"
        )
    if profile["split_encoding"] == "opset13-static-input":
        _validate_opset13_split_input(
            split_index,
            split_node,
            nodes_by_name,
            producers,
            consumers,
            onnx,
        )

    expected_split_contracts = (
        ([1, 65, profile["candidate_count"]], split_node.input[0]),
        (profile["regression_output_shape"], split_node.output[0]),
        (profile["class_output_shape"], split_node.output[1]),
    )
    for expected_shape, tensor_name in expected_split_contracts:
        observed = _tensor_contract(_find_value_info(inferred_model, tensor_name))
        if observed["dtype"] != "float32" or observed["shape"] != expected_shape:
            raise ValueError(
                f"predecode tensor {tensor_name!r} must be float32 {expected_shape}: "
                f"{observed}"
            )
    for source_tensor in _PREDECODE_SOURCE_TENSORS:
        if producers[source_tensor] != [(split_index, split_node)]:
            raise ValueError(
                f"predecode tensor {source_tensor!r} must be produced only by "
                "/model.22/Split"
            )
    expected_source_consumer_names = {
        _PREDECODE_SOURCE_TENSORS[0]: "/model.22/dfl/Reshape",
        _PREDECODE_SOURCE_TENSORS[1]: "/model.22/Sigmoid",
    }
    for source_tensor, expected_consumer_name in expected_source_consumer_names.items():
        matches = nodes_by_name[expected_consumer_name]
        if len(matches) != 1:
            raise ValueError(
                f"expected exactly one decode-tail node {expected_consumer_name!r}, "
                f"found {len(matches)}"
            )
        expected_consumers = [(matches[0][0], 0)]
        if consumers[source_tensor] != expected_consumers:
            raise ValueError(
                f"predecode tensor {source_tensor!r} consumers mismatch: "
                f"expected={expected_consumers} actual={consumers[source_tensor]}"
            )

    initializer_names = {initializer.name for initializer in graph.initializer}
    graph_input_names = {item.name for item in graph.input}
    pending = [graph.output[0].name]
    reachable_tail_indexes: set[int] = set()
    while pending:
        tensor_name = pending.pop()
        if tensor_name in _PREDECODE_SOURCE_TENSORS:
            continue
        if tensor_name in initializer_names or tensor_name in graph_input_names:
            continue
        tensor_producers = producers[tensor_name]
        if len(tensor_producers) != 1:
            raise ValueError(
                f"decode-tail tensor {tensor_name!r} must have exactly one producer, "
                f"found {len(tensor_producers)}"
            )
        node_index, node = tensor_producers[0]
        if node_index in reachable_tail_indexes:
            continue
        reachable_tail_indexes.add(node_index)
        pending.extend(name for name in node.input if name)

    tail_node_specs = profile["tail_node_specs"]
    expected_tail_names = set(tail_node_specs)
    reachable_tail_names = {graph.node[index].name for index in reachable_tail_indexes}
    if reachable_tail_names != expected_tail_names:
        raise ValueError(
            "reachable YOLOv8 decode tail mismatch: "
            f"expected={sorted(expected_tail_names)} "
            f"actual={sorted(reachable_tail_names)}"
        )

    for node_name, expected_spec in tail_node_specs.items():
        matches = nodes_by_name[node_name]
        if len(matches) != 1:
            raise ValueError(
                f"expected exactly one decode-tail node {node_name!r}, "
                f"found {len(matches)}"
            )
        node_index, node = matches[0]
        observed_spec = (node.op_type, list(node.input), list(node.output))
        if observed_spec != expected_spec or node.domain:
            raise ValueError(
                f"decode-tail node {node_name!r} topology mismatch: {observed_spec}"
            )
        observed_attributes = _node_attributes(node, onnx)
        expected_attributes = profile["tail_attribute_specs"].get(node_name, {})
        if node.op_type == "Constant" and profile["constants_in_nodes"]:
            if set(observed_attributes) != {"value"}:
                raise ValueError(
                    f"decode-tail Constant {node_name!r} must contain only value"
                )
        elif observed_attributes != expected_attributes:
            raise ValueError(
                f"decode-tail node {node_name!r} attributes mismatch: "
                f"expected={expected_attributes} actual={observed_attributes}"
            )
        for output_name in node.output:
            unexpected_consumers = [
                consumer_index
                for consumer_index, _ in consumers[output_name]
                if consumer_index not in reachable_tail_indexes
            ]
            if unexpected_consumers:
                raise ValueError(
                    f"decode-tail tensor {output_name!r} has consumers outside the "
                    f"removed tail: {unexpected_consumers}"
                )

    _validate_predecode_initializers(model, onnx, profile)
    retained_node_inputs = {
        input_name
        for node_index, node in enumerate(graph.node)
        if node_index not in reachable_tail_indexes
        for input_name in node.input
        if input_name
    }
    unused_initializer_names = {
        initializer.name
        for initializer in graph.initializer
        if initializer.name not in retained_node_inputs
    }
    expected_unused = set(profile["unused_initializers"])
    if unused_initializer_names != expected_unused:
        raise ValueError(
            "unused initializer set after decode-tail removal mismatch: "
            f"expected={sorted(expected_unused)} "
            f"actual={sorted(unused_initializer_names)}"
        )

    anchor_points, strides = build_yolov8_anchor_grid(profile["model_size"])
    dfl_projection = (
        QAT_FULL320_DFL_PROJECTION
        if profile["constants_in_nodes"]
        else np.arange(16, dtype=np.float32)
    )
    dfl_weights = dfl_projection.reshape(1, 16, 1, 1)
    return {
        "tail_node_indexes": reachable_tail_indexes,
        "tail_node_names": [
            node.name
            for node_index, node in enumerate(graph.node)
            if node_index in reachable_tail_indexes
        ],
        "unused_initializer_names": [
            initializer.name
            for initializer in graph.initializer
            if initializer.name in unused_initializer_names
        ],
        "anchor_grid_sha256": _array_sha256(anchor_points),
        "stride_sha256": _array_sha256(strides),
        "dfl_weight_sha256": _array_sha256(dfl_weights),
        "split_encoding": profile["split_encoding"],
    }


def _validate_source_contract(
    graph: Any,
    inferred_model: Any,
    *,
    input_shape: list[int] | None = None,
    output_shape: list[int] | None = None,
) -> None:
    if len(graph.input) != 1:
        raise ValueError(f"expected exactly one model input, found {len(graph.input)}")
    inferred_input = _find_value_info(inferred_model, graph.input[0].name)
    expected_input = {
        "name": "images",
        "dtype": "float32",
        "shape": input_shape or [1, 3, 640, 640],
    }
    if _tensor_contract(inferred_input) != expected_input:
        raise ValueError(
            f"source input contract mismatch: {_tensor_contract(inferred_input)}"
        )
    if len(graph.output) != 1:
        raise ValueError(f"expected exactly one source output, found {len(graph.output)}")
    source_output = _tensor_contract(inferred_model.graph.output[0])
    expected_output_shape = output_shape or SOURCE_OUTPUT_SHAPE
    if (
        source_output["dtype"] != "float32"
        or source_output["shape"] != expected_output_shape
    ):
        raise ValueError(
            f"source output must be float32 {expected_output_shape}: {source_output}"
        )


def _validate_opset13_split_input(
    split_index: int,
    split_node: Any,
    nodes_by_name: dict[str, list[tuple[int, Any]]],
    producers: dict[str, list[tuple[int, Any]]],
    consumers: dict[str, list[tuple[int, int]]],
    onnx: Any,
) -> None:
    constant_matches = nodes_by_name["Constant_247"]
    if len(constant_matches) != 1:
        raise ValueError(
            "opset-13 static Split requires exactly one Constant_247 producer"
        )
    constant_index, constant_node = constant_matches[0]
    expected_spec = ("Constant", [], ["onnx::Split_388"])
    observed_spec = (
        constant_node.op_type,
        list(constant_node.input),
        list(constant_node.output),
    )
    if observed_spec != expected_spec or constant_node.domain:
        raise ValueError(
            f"opset-13 static Split constant topology mismatch: {observed_spec}"
        )
    if producers["onnx::Split_388"] != [(constant_index, constant_node)]:
        raise ValueError("opset-13 static Split input must have one producer")
    if consumers["onnx::Split_388"] != [(split_index, 1)]:
        raise ValueError("opset-13 static Split input must feed only /model.22/Split")
    attributes = _node_attributes(constant_node, onnx)
    if set(attributes) != {"value"}:
        raise ValueError("opset-13 static Split Constant must contain only value")
    split_values = onnx.numpy_helper.to_array(attributes["value"])
    expected_values = np.asarray([64, 1], dtype=np.int64)
    if not np.array_equal(split_values, expected_values):
        raise ValueError(
            "opset-13 static Split input must be int64 [64, 1], "
            f"got dtype={split_values.dtype} values={split_values.tolist()}"
        )
    if list(split_node.input) != ["/model.22/Concat_3_output_0", "onnx::Split_388"]:
        raise ValueError("opset-13 static Split inputs changed during validation")


def _validate_predecode_initializers(
    model: Any,
    onnx: Any,
    profile: dict[str, Any],
) -> None:
    candidate_count = profile["candidate_count"]
    anchor_points, strides = build_yolov8_anchor_grid(profile["model_size"])
    if profile["constants_in_nodes"]:
        _validate_opset13_decode_constants(model, onnx, anchor_points, strides)
        return
    expected_values = {
        "/model.22/dfl/Constant_output_0": np.asarray(
            [1, 4, 16, candidate_count], dtype=np.int64
        ),
        "model.22.dfl.conv.weight": np.arange(16, dtype=np.float32).reshape(
            1, 16, 1, 1
        ),
        "/model.22/dfl/Constant_1_output_0": np.asarray(
            [1, 4, candidate_count], dtype=np.int64
        ),
        "/model.22/Constant_4_output_0": np.asarray([0], dtype=np.int64),
        "/model.22/Constant_9_output_0": anchor_points,
        "/model.22/Constant_10_output_0": anchor_points,
        "/model.22/Constant_11_output_0": np.asarray(2.0, dtype=np.float32),
        "/model.22/Constant_12_output_0": strides,
    }
    if profile["model_size"] == 640:
        expected_values.update(
            {
                "/model.22/Gather_output_0": np.asarray([4], dtype=np.int64),
                "/model.22/Constant_5_output_0": np.asarray([1], dtype=np.int64),
                "/model.22/Constant_6_output_0": np.asarray([2], dtype=np.int64),
            }
        )
    else:
        expected_values.update(
            {
                "/model.22/Mul_output_0": np.asarray([2], dtype=np.int64),
                "/model.22/Constant_3_output_0": np.asarray([1], dtype=np.int64),
                "/model.22/Mul_1_output_0": np.asarray([4], dtype=np.int64),
            }
        )
    initializers_by_name: dict[str, list[Any]] = defaultdict(list)
    for initializer in model.graph.initializer:
        initializers_by_name[initializer.name].append(initializer)
    for initializer_name, expected in expected_values.items():
        matches = initializers_by_name[initializer_name]
        if len(matches) != 1:
            raise ValueError(
                f"expected exactly one initializer {initializer_name!r}, "
                f"found {len(matches)}"
            )
        observed = onnx.numpy_helper.to_array(matches[0])
        if observed.dtype != expected.dtype or not np.array_equal(observed, expected):
            raise ValueError(
                f"decode initializer {initializer_name!r} does not match the "
                f"frozen YOLOv8 full{profile['model_size']} decoder contract"
            )


def _validate_opset13_decode_constants(
    model: Any,
    onnx: Any,
    anchor_points: np.ndarray,
    strides: np.ndarray,
) -> None:
    expected_values = {
        "/model.22/dfl/Constant": np.asarray([1, 4, 16, 2100], dtype=np.int64),
        "/model.22/dfl/Constant_1": np.asarray([1, 4, 2100], dtype=np.int64),
        "/model.22/Constant_3": np.asarray([1], dtype=np.int64),
        "/model.22/Constant_4": np.asarray([0], dtype=np.int64),
        "/model.22/Constant_5": np.asarray([1], dtype=np.int64),
        "/model.22/Constant_6": np.asarray([2], dtype=np.int64),
        "/model.22/Constant_7": np.asarray([1], dtype=np.int64),
        "/model.22/Constant_8": np.asarray([2], dtype=np.int64),
        "/model.22/Constant_9": anchor_points,
        "/model.22/Constant_10": anchor_points,
        "/model.22/Constant_11": np.asarray(2.0, dtype=np.float32),
        "/model.22/Constant_12": strides,
    }
    nodes_by_name: dict[str, list[Any]] = defaultdict(list)
    for node in model.graph.node:
        nodes_by_name[node.name].append(node)
    for node_name, expected in expected_values.items():
        matches = nodes_by_name[node_name]
        if len(matches) != 1:
            raise ValueError(
                f"expected exactly one decode Constant node {node_name!r}, "
                f"found {len(matches)}"
            )
        attributes = _node_attributes(matches[0], onnx)
        if set(attributes) != {"value"}:
            raise ValueError(
                f"decode Constant node {node_name!r} must contain only value"
            )
        observed = onnx.numpy_helper.to_array(attributes["value"])
        if observed.dtype != expected.dtype or not np.array_equal(observed, expected):
            raise ValueError(
                f"decode Constant node {node_name!r} does not match the frozen "
                "YOLOv8 full320 opset-13 decoder contract"
            )

    dfl_matches = [
        initializer
        for initializer in model.graph.initializer
        if initializer.name == "model.22.dfl.conv.weight"
    ]
    if len(dfl_matches) != 1:
        raise ValueError("expected exactly one model.22.dfl.conv.weight initializer")
    observed_dfl = onnx.numpy_helper.to_array(dfl_matches[0])
    expected_dfl = QAT_FULL320_DFL_PROJECTION.reshape(1, 16, 1, 1)
    if observed_dfl.dtype != expected_dfl.dtype or not np.array_equal(
        observed_dfl, expected_dfl
    ):
        raise ValueError("DFL projection weight does not match the frozen QAT source")


def _node_attributes(node: Any, onnx: Any) -> dict[str, Any]:
    return {
        attribute.name: onnx.helper.get_attribute_value(attribute)
        for attribute in node.attribute
    }


def _array_sha256(values: np.ndarray) -> str:
    return hashlib.sha256(values.tobytes()).hexdigest()


def _prune_stale_value_info(model: Any) -> None:
    live_names = {
        name
        for node in model.graph.node
        for name in (*node.input, *node.output)
        if name
    }
    live_names.update(item.name for item in model.graph.input)
    live_names.update(item.name for item in model.graph.output)
    live_names.update(item.name for item in model.graph.initializer)
    retained = [
        value_info
        for value_info in model.graph.value_info
        if value_info.name in live_names
    ]
    del model.graph.value_info[:]
    model.graph.value_info.extend(retained)


def _validate_terminal_concat(
    model: Any,
    inferred_model: Any,
    onnx: Any,
) -> dict[str, Any]:
    graph = model.graph
    profile = _select_split_output_profile(graph, inferred_model)

    producers: dict[str, list[tuple[int, Any]]] = defaultdict(list)
    consumers: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for node_index, node in enumerate(graph.node):
        for output_name in node.output:
            producers[output_name].append((node_index, node))
        for input_index, input_name in enumerate(node.input):
            consumers[input_name].append((node_index, input_index))

    output_name = graph.output[0].name
    output_producers = producers[output_name]
    if len(output_producers) != 1:
        raise ValueError(
            f"source output must have exactly one producer, found {len(output_producers)}"
        )
    final_node_index, final_node = output_producers[0]
    if final_node_index != len(graph.node) - 1:
        raise ValueError("terminal output producer must be the final graph node")
    if final_node.op_type != "Concat" or len(final_node.input) != 2:
        raise ValueError("source output must be produced by a two-input Concat")
    if list(final_node.output) != [output_name]:
        raise ValueError("terminal Concat must produce only the source graph output")
    axis_attributes = [
        onnx.helper.get_attribute_value(attribute)
        for attribute in final_node.attribute
        if attribute.name == "axis"
    ]
    if axis_attributes != [1]:
        raise ValueError(f"terminal Concat axis must be 1, got {axis_attributes}")

    expected_inputs = [
        profile["boxes_output_shape"],
        profile["scores_output_shape"],
    ]
    for input_index, (input_name, expected_shape) in enumerate(
        zip(final_node.input, expected_inputs)
    ):
        value_info = _find_value_info(inferred_model, input_name)
        observed = _tensor_contract(value_info)
        if observed["dtype"] != "float32" or observed["shape"] != expected_shape:
            raise ValueError(
                f"terminal Concat input {input_index} must be float32 {expected_shape}: "
                f"{observed}"
            )
        if len(producers[input_name]) != 1:
            raise ValueError(
                f"terminal Concat input {input_name!r} must have exactly one producer"
            )
        if consumers[input_name] != [(final_node_index, input_index)]:
            raise ValueError(
                f"terminal Concat input {input_name!r} must feed only the final Concat"
            )

    return {
        "model_size": profile["model_size"],
        "candidate_count": profile["candidate_count"],
        "boxes_output_shape": profile["boxes_output_shape"],
        "scores_output_shape": profile["scores_output_shape"],
        "final_node_index": final_node_index,
        "removed_node_name": final_node.name,
        "boxes_source_name": final_node.input[0],
        "scores_source_name": final_node.input[1],
    }


def _select_split_output_profile(
    graph: Any,
    inferred_model: Any,
) -> dict[str, Any]:
    if len(graph.input) != 1 or len(graph.output) != 1:
        raise ValueError("split-output source must have exactly one input and output")
    input_contract = _tensor_contract(
        _find_value_info(inferred_model, graph.input[0].name)
    )
    output_contract = _tensor_contract(inferred_model.graph.output[0])
    for profile in _SPLIT_OUTPUT_PROFILES:
        if (
            input_contract
            == {
                "name": "images",
                "dtype": "float32",
                "shape": profile["input_shape"],
            }
            and output_contract["dtype"] == "float32"
            and output_contract["shape"] == profile["source_output_shape"]
        ):
            _validate_source_contract(
                graph,
                inferred_model,
                input_shape=profile["input_shape"],
                output_shape=profile["source_output_shape"],
            )
            return profile
    raise ValueError(
        "unsupported split-output source contract: "
        f"input={input_contract}, output={output_contract}"
    )


def _find_value_info(model: Any, name: str) -> Any:
    matches = [
        item
        for collection in (model.graph.input, model.graph.output, model.graph.value_info)
        for item in collection
        if item.name == name
    ]
    if not matches:
        raise ValueError(f"shape inference did not describe tensor {name!r}")
    return matches[0]


def _tensor_contract(value_info: Any) -> dict[str, Any]:
    tensor_type = value_info.type.tensor_type
    dtype_by_id = {1: "float32", 2: "uint8", 3: "int8"}
    shape: list[int | str] = []
    for dimension in tensor_type.shape.dim:
        if dimension.HasField("dim_value"):
            shape.append(dimension.dim_value)
        else:
            shape.append(dimension.dim_param)
    return {
        "name": value_info.name,
        "dtype": dtype_by_id.get(
            tensor_type.elem_type,
            f"onnx_type_{tensor_type.elem_type}",
        ),
        "shape": shape,
    }


def _require_available_output_names(
    model: Any,
    replaced_names: set[str],
    new_names: set[str],
) -> None:
    existing_names = {
        name
        for node in model.graph.node
        for name in (*node.input, *node.output)
        if name
    }
    existing_names.update(item.name for item in model.graph.input)
    existing_names.update(item.name for item in model.graph.output)
    existing_names.update(item.name for item in model.graph.value_info)
    existing_names.update(item.name for item in model.graph.initializer)
    collisions = (new_names & existing_names) - replaced_names
    if collisions:
        raise ValueError(f"derived output names already exist: {sorted(collisions)}")


def _rename_tensor(model: Any, old_name: str, new_name: str) -> None:
    renamed_producer_outputs = 0
    for node in model.graph.node:
        for output_index, output_name in enumerate(node.output):
            if output_name == old_name:
                node.output[output_index] = new_name
                renamed_producer_outputs += 1
    if renamed_producer_outputs != 1:
        raise ValueError(
            f"expected one producer output named {old_name!r}, found "
            f"{renamed_producer_outputs}"
        )
    for value_info in model.graph.value_info:
        if value_info.name == old_name:
            value_info.name = new_name
    for annotation in model.graph.quantization_annotation:
        if annotation.tensor_name == old_name:
            annotation.tensor_name = new_name
