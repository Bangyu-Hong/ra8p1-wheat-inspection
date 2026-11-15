"""Guarded final-Conv output transform for the QAT YOLOv8n candidate."""

from __future__ import annotations

from collections import defaultdict
import hashlib
from pathlib import Path
from typing import Any

import numpy as np

from .contract import sha256_file


SIX_HEAD_TRANSFORM_ID = "expose-yolov8-final-conv-heads-v1"

_CONV_ATTRIBUTES = {
    "dilations": [1, 1],
    "group": 1,
    "kernel_shape": [1, 1],
    "pads": [0, 0, 0, 0],
    "strides": [1, 1],
}

_SCALE_SPECS = (
    {
        "scale_index": 0,
        "stride": 8,
        "spatial": 40,
        "concat_name": "/model.22/Concat",
        "regression_weight_sha256": (
            "8c54f687a0dd1dda5067903c440867aa74a1ee66955d2d9dbf1bae9e77442985"
        ),
        "regression_bias_sha256": (
            "7695b065002cee28f4fc554e64621dbafb764c18fead99189151c92ad748f321"
        ),
        "class_weight_sha256": (
            "fc4bca4eaa44c90fa95d8c1892a03a4c9f6d547c3e5da24556ffd7882d03758b"
        ),
        "class_bias_sha256": (
            "c7a2be4c93921ec2138341ae3ad086424cbae305fe45d34f9fc8276204038aa9"
        ),
    },
    {
        "scale_index": 1,
        "stride": 16,
        "spatial": 20,
        "concat_name": "/model.22/Concat_1",
        "regression_weight_sha256": (
            "cded6d3f0c80500a854dab44d48649f81f33ae47510d0aae8cc0587538ee6e8f"
        ),
        "regression_bias_sha256": (
            "5138633dca935fefe5f21ac7cdb67d5547043e78e4bc3ee2fda8c0ba759b7622"
        ),
        "class_weight_sha256": (
            "34838d58a80148815659294cd41b8ddea7f433ff205bf19ffec33041eaad30c8"
        ),
        "class_bias_sha256": (
            "a93e11e243cd0afd6f06b64ed64b18b91e392ca2e9c47b04c8fdbc6df9b3e73f"
        ),
    },
    {
        "scale_index": 2,
        "stride": 32,
        "spatial": 10,
        "concat_name": "/model.22/Concat_2",
        "regression_weight_sha256": (
            "c1682ba91bc5e69ac6eb817f6390a6d638d865c69350ad7fad4b90050ff8c443"
        ),
        "regression_bias_sha256": (
            "45f7750f6175815ef6275c4fb90c1654453a1553def728760e0e68096a72bdad"
        ),
        "class_weight_sha256": (
            "3f80824a09c05bb20eb9f702bd2a7819d78389612b748464d64c0e18a2bbdbd1"
        ),
        "class_bias_sha256": (
            "075713c97f01082d8c25296bad25cfdd25e1c68ded7a1f9a68ce12aa92478d2e"
        ),
    },
)


def _build_output_specs() -> tuple[dict[str, Any], ...]:
    outputs: list[dict[str, Any]] = []
    for scale in _SCALE_SPECS:
        index = scale["scale_index"]
        spatial = scale["spatial"]
        for branch, source_branch, channels in (
            ("regression", "cv2", 64),
            ("class", "cv3", 1),
        ):
            prefix = f"/model.22/{source_branch}.{index}/{source_branch}.{index}"
            outputs.append(
                {
                    "name": f"{branch}_s{scale['stride']}",
                    "branch": branch,
                    "stride": scale["stride"],
                    "node_name": f"{prefix}.2/Conv",
                    "source_tensor": f"{prefix}.2/Conv_output_0",
                    "activation_input": f"{prefix}.1/act/Mul_output_0",
                    "weight_name": f"model.22.{source_branch}.{index}.2.weight",
                    "bias_name": f"model.22.{source_branch}.{index}.2.bias",
                    "shape": [1, channels, spatial, spatial],
                    "weight_shape": [channels, 64, 1, 1],
                    "bias_shape": [channels],
                    "weight_sha256": scale[f"{branch}_weight_sha256"],
                    "bias_sha256": scale[f"{branch}_bias_sha256"],
                    "consumer_name": scale["concat_name"],
                }
            )
    return tuple(outputs)


SIX_HEAD_OUTPUT_SPECS = _build_output_specs()
SIX_HEAD_OUTPUT_NAMES = tuple(item["name"] for item in SIX_HEAD_OUTPUT_SPECS)


def derive_six_head_output_model(
    source_path: Path,
    output_path: Path,
) -> dict[str, Any]:
    """Expose six final detection Conv tensors and prune later packing/decode."""

    if source_path.resolve() == output_path.resolve():
        raise ValueError("source and derived output paths must differ")
    try:
        import onnx
    except ImportError as exc:
        raise RuntimeError("onnx is required for six-head model derivation") from exc

    source_sha256 = sha256_file(source_path)
    model = onnx.load(str(source_path), load_external_data=False)
    onnx.checker.check_model(model)
    try:
        inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    except Exception as exc:
        raise ValueError(f"source shape inference failed: {exc}") from exc

    topology = _validate_six_head_source(model, inferred, onnx)
    retained_indexes = _backward_reachable_node_indexes(
        model.graph,
        {item["source_tensor"] for item in SIX_HEAD_OUTPUT_SPECS},
    )
    retained_nodes = [
        node
        for node_index, node in enumerate(model.graph.node)
        if node_index in retained_indexes
    ]
    removed_node_names = [
        node.name
        for node_index, node in enumerate(model.graph.node)
        if node_index not in retained_indexes
    ]
    del model.graph.node[:]
    model.graph.node.extend(retained_nodes)

    used_inputs = {name for node in retained_nodes for name in node.input if name}
    retained_initializers = [
        initializer
        for initializer in model.graph.initializer
        if initializer.name in used_inputs
    ]
    removed_initializer_names = [
        initializer.name
        for initializer in model.graph.initializer
        if initializer.name not in used_inputs
    ]
    del model.graph.initializer[:]
    model.graph.initializer.extend(retained_initializers)

    rename_map = {
        item["source_tensor"]: item["name"] for item in SIX_HEAD_OUTPUT_SPECS
    }
    _rename_tensors(model, rename_map)
    del model.graph.output[:]
    model.graph.output.extend(
        [
            onnx.helper.make_tensor_value_info(
                item["name"],
                onnx.TensorProto.FLOAT,
                item["shape"],
            )
            for item in SIX_HEAD_OUTPUT_SPECS
        ]
    )
    _prune_stale_value_info(model)

    metadata = {item.key: item.value for item in model.metadata_props}
    metadata.update(
        {
            "renesas_cup.transform": SIX_HEAD_TRANSFORM_ID,
            "renesas_cup.transform.output_order": ",".join(SIX_HEAD_OUTPUT_NAMES),
            "renesas_cup.transform.output_assembly": (
                "row-major reshape per scale; concatenate s8,s16,s32"
            ),
            "renesas_cup.transform.source_sha256": source_sha256,
        }
    )
    onnx.helper.set_model_props(model, metadata)
    onnx.checker.check_model(model)
    derived_inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    observed_outputs = [
        _tensor_contract(item) for item in derived_inferred.graph.output
    ]
    expected_outputs = [
        {"name": item["name"], "dtype": "float32", "shape": item["shape"]}
        for item in SIX_HEAD_OUTPUT_SPECS
    ]
    if observed_outputs != expected_outputs:
        raise ValueError(f"six-head output contract mismatch: {observed_outputs}")

    output_producers = {
        output.name: [
            node for node in model.graph.node if output.name in node.output
        ]
        for output in model.graph.output
    }
    if any(
        len(nodes) != 1 or nodes[0].op_type != "Conv"
        for nodes in output_producers.values()
    ):
        raise ValueError("every six-head output must be produced directly by Conv")

    serialized = model.SerializeToString(deterministic=True)
    derived_sha256 = hashlib.sha256(serialized).hexdigest()
    if output_path.exists():
        if output_path.read_bytes() != serialized:
            raise FileExistsError(
                f"refusing to replace non-matching six-head model: {output_path}"
            )
    else:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(serialized)

    return {
        "transform_id": SIX_HEAD_TRANSFORM_ID,
        "source_bytes": source_path.stat().st_size,
        "source_sha256": source_sha256,
        "derived_bytes": len(serialized),
        "derived_sha256": derived_sha256,
        "retained_node_count": len(retained_nodes),
        "removed_node_count": len(removed_node_names),
        "removed_node_names": removed_node_names,
        "retained_initializer_count": len(retained_initializers),
        "removed_initializer_names": removed_initializer_names,
        "final_conv_artifacts": topology["final_conv_artifacts"],
        "outputs": expected_outputs,
    }


def _validate_six_head_source(
    model: Any,
    inferred: Any,
    onnx: Any,
) -> dict[str, Any]:
    graph = model.graph
    if len(graph.input) != 1 or len(graph.output) != 1:
        raise ValueError("six-head source must have exactly one input and one output")
    input_contract = _tensor_contract(_find_value_info(inferred, graph.input[0].name))
    output_contract = _tensor_contract(inferred.graph.output[0])
    if input_contract != {
        "name": "images",
        "dtype": "float32",
        "shape": [1, 3, 320, 320],
    }:
        raise ValueError(f"six-head source input mismatch: {input_contract}")
    if output_contract != {
        "name": "output0",
        "dtype": "float32",
        "shape": [1, 5, 2100],
    }:
        raise ValueError(f"six-head source output mismatch: {output_contract}")

    nodes_by_name: dict[str, list[Any]] = defaultdict(list)
    consumers: dict[str, list[Any]] = defaultdict(list)
    for node in graph.node:
        nodes_by_name[node.name].append(node)
        for input_name in node.input:
            consumers[input_name].append(node)
    initializers_by_name: dict[str, list[Any]] = defaultdict(list)
    for initializer in graph.initializer:
        initializers_by_name[initializer.name].append(initializer)

    final_conv_artifacts = []
    for spec in SIX_HEAD_OUTPUT_SPECS:
        matches = nodes_by_name[spec["node_name"]]
        if len(matches) != 1:
            raise ValueError(
                f"expected one final Conv {spec['node_name']!r}, found {len(matches)}"
            )
        node = matches[0]
        expected_inputs = [
            spec["activation_input"],
            spec["weight_name"],
            spec["bias_name"],
        ]
        if (
            node.op_type != "Conv"
            or node.domain
            or list(node.input) != expected_inputs
            or list(node.output) != [spec["source_tensor"]]
        ):
            raise ValueError(f"final Conv topology mismatch: {spec['node_name']}")
        if _node_attributes(node, onnx) != _CONV_ATTRIBUTES:
            raise ValueError(f"final Conv attributes mismatch: {spec['node_name']}")
        observed_tensor = _tensor_contract(
            _find_value_info(inferred, spec["source_tensor"])
        )
        if observed_tensor["dtype"] != "float32" or observed_tensor["shape"] != spec["shape"]:
            raise ValueError(
                f"final Conv tensor contract mismatch: {observed_tensor}"
            )
        tensor_consumers = consumers[spec["source_tensor"]]
        if len(tensor_consumers) != 1 or tensor_consumers[0].name != spec["consumer_name"]:
            raise ValueError(
                f"final Conv consumer mismatch: {spec['source_tensor']}"
            )

        artifacts = {}
        for kind in ("weight", "bias"):
            initializer_name = spec[f"{kind}_name"]
            initializer_matches = initializers_by_name[initializer_name]
            if len(initializer_matches) != 1:
                raise ValueError(
                    f"expected one initializer {initializer_name!r}, "
                    f"found {len(initializer_matches)}"
                )
            values = np.asarray(onnx.numpy_helper.to_array(initializer_matches[0]))
            observed_sha256 = hashlib.sha256(
                np.ascontiguousarray(values).tobytes()
            ).hexdigest()
            if (
                values.dtype != np.float32
                or list(values.shape) != spec[f"{kind}_shape"]
                or observed_sha256 != spec[f"{kind}_sha256"]
            ):
                raise ValueError(
                    f"frozen final Conv {kind} mismatch: {initializer_name}"
                )
            artifacts[kind] = {
                "name": initializer_name,
                "shape": list(values.shape),
                "dtype": str(values.dtype),
                "tensor_sha256": observed_sha256,
            }
        final_conv_artifacts.append(
            {
                "output": spec["name"],
                "node_name": spec["node_name"],
                **artifacts,
            }
        )

    for scale in _SCALE_SPECS:
        index = scale["scale_index"]
        suffix = "" if index == 0 else f"_{index}"
        concat_matches = nodes_by_name[scale["concat_name"]]
        expected_inputs = [
            f"/model.22/cv2.{index}/cv2.{index}.2/Conv_output_0",
            f"/model.22/cv3.{index}/cv3.{index}.2/Conv_output_0",
        ]
        expected_output = f"/model.22/Concat{suffix}_output_0"
        if len(concat_matches) != 1:
            raise ValueError(f"missing frozen head Concat {scale['concat_name']}")
        concat = concat_matches[0]
        if (
            concat.op_type != "Concat"
            or concat.domain
            or list(concat.input) != expected_inputs
            or list(concat.output) != [expected_output]
            or _node_attributes(concat, onnx) != {"axis": 1}
        ):
            raise ValueError(f"head packing Concat mismatch: {scale['concat_name']}")

    return {"final_conv_artifacts": final_conv_artifacts}


def _backward_reachable_node_indexes(
    graph: Any,
    output_tensors: set[str],
) -> set[int]:
    producers: dict[str, list[int]] = defaultdict(list)
    for node_index, node in enumerate(graph.node):
        for output_name in node.output:
            producers[output_name].append(node_index)
    retained: set[int] = set()
    pending = list(output_tensors)
    while pending:
        tensor_name = pending.pop()
        matches = producers.get(tensor_name, [])
        if len(matches) > 1:
            raise ValueError(f"tensor has multiple producers: {tensor_name}")
        if not matches:
            continue
        node_index = matches[0]
        if node_index in retained:
            continue
        retained.add(node_index)
        pending.extend(name for name in graph.node[node_index].input if name)
    return retained


def _rename_tensors(model: Any, rename_map: dict[str, str]) -> None:
    existing_names = {
        name
        for node in model.graph.node
        for name in [*node.input, *node.output]
        if name
    }
    collisions = set(rename_map.values()) & (existing_names - set(rename_map))
    if collisions:
        raise ValueError(f"six-head output name collision: {sorted(collisions)}")
    for node in model.graph.node:
        for index, name in enumerate(node.input):
            if name in rename_map:
                node.input[index] = rename_map[name]
        for index, name in enumerate(node.output):
            if name in rename_map:
                node.output[index] = rename_map[name]
    for value_info in model.graph.value_info:
        if value_info.name in rename_map:
            value_info.name = rename_map[value_info.name]


def _prune_stale_value_info(model: Any) -> None:
    live_names = {
        name
        for node in model.graph.node
        for name in [*node.input, *node.output]
        if name
    }
    live_names.update(item.name for item in model.graph.input)
    live_names.update(item.name for item in model.graph.output)
    retained = [
        value_info
        for value_info in model.graph.value_info
        if value_info.name in live_names
    ]
    del model.graph.value_info[:]
    model.graph.value_info.extend(retained)


def _find_value_info(model: Any, name: str) -> Any:
    matches = [
        value
        for value in [
            *model.graph.input,
            *model.graph.output,
            *model.graph.value_info,
        ]
        if value.name == name
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one value-info entry for {name!r}")
    return matches[0]


def _tensor_contract(value_info: Any) -> dict[str, Any]:
    tensor_type = value_info.type.tensor_type
    dtype_by_id = {1: "float32", 2: "uint8", 3: "int8"}
    shape = [
        dimension.dim_value
        if dimension.HasField("dim_value")
        else dimension.dim_param
        for dimension in tensor_type.shape.dim
    ]
    return {
        "name": value_info.name,
        "dtype": dtype_by_id.get(
            tensor_type.elem_type, f"onnx_type_{tensor_type.elem_type}"
        ),
        "shape": shape,
    }


def _node_attributes(node: Any, onnx: Any) -> dict[str, Any]:
    return {
        attribute.name: onnx.helper.get_attribute_value(attribute)
        for attribute in node.attribute
    }
