"""Immutable model-contract loading and verification."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
from typing import Any


CONTRACT_PATH = Path(__file__).with_name("manifests") / "yolo11n_wheat_full640.json"


def sha256_file(path: Path, chunk_bytes: int = 1024 * 1024) -> str:
    """Return a lowercase SHA-256 digest without loading a large file at once."""

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_bytes):
            digest.update(chunk)
    return digest.hexdigest()


def load_model_contract(path: Path | None = None) -> dict[str, Any]:
    contract_path = path or CONTRACT_PATH
    with contract_path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def get_output_contracts(contract: dict[str, Any]) -> list[dict[str, Any]]:
    """Return ordered output contracts from legacy or split-output manifests."""

    has_output = "output" in contract
    has_outputs = "outputs" in contract
    if has_output == has_outputs:
        raise ValueError("model contract must define exactly one of 'output' or 'outputs'")
    outputs = [contract["output"]] if has_output else contract["outputs"]
    if not isinstance(outputs, list) or not outputs:
        raise ValueError("model contract outputs must be a non-empty list")
    if not all(isinstance(item, dict) for item in outputs):
        raise ValueError("each model output contract must be an object")
    return outputs


def verify_model_contract(model_path: Path, contract_path: Path | None = None) -> dict[str, Any]:
    """Verify model identity and, when available, its ONNX tensor contract."""

    contract = load_model_contract(contract_path)
    source = contract["source_model"]
    actual_bytes = model_path.stat().st_size
    actual_sha256 = sha256_file(model_path)
    errors: list[str] = []

    if actual_bytes != source["bytes"]:
        errors.append(f"size expected={source['bytes']} actual={actual_bytes}")
    if actual_sha256 != source["sha256"]:
        errors.append(f"sha256 expected={source['sha256']} actual={actual_sha256}")

    try:
        import onnx
    except ImportError:
        onnx = None

    observed: dict[str, Any] = {
        "bytes": actual_bytes,
        "sha256": actual_sha256,
        "onnx_checked": onnx is not None,
    }
    if onnx is not None:
        model = onnx.load(str(model_path), load_external_data=False)
        onnx.checker.check_model(model)
        observed["opsets"] = {
            item.domain or "ai.onnx": item.version for item in model.opset_import
        }
        observed["ir_version"] = model.ir_version
        observed["producer_name"] = model.producer_name
        observed["producer_version"] = model.producer_version
        observed["operator_counts"] = dict(
            sorted(Counter(node.op_type for node in model.graph.node).items())
        )
        observed["metadata"] = {
            item.key: item.value for item in model.metadata_props
        }
        observed["inputs"] = [_tensor_value_info(item) for item in model.graph.input]
        observed["outputs"] = [_tensor_value_info(item) for item in model.graph.output]
        observed["dynamic_interface_axes"] = any(
            not dimension.HasField("dim_value")
            for item in [*model.graph.input, *model.graph.output]
            for dimension in item.type.tensor_type.shape.dim
        )
        tensor_types = [
            item.type.tensor_type.elem_type
            for item in [
                *model.graph.input,
                *model.graph.output,
                *model.graph.value_info,
            ]
        ]
        initializer_types = [item.data_type for item in model.graph.initializer]
        constant_types = [
            attribute.t.data_type
            for node in model.graph.node
            for attribute in node.attribute
            if attribute.type == onnx.AttributeProto.TENSOR
        ]
        observed["fp16_present"] = onnx.TensorProto.FLOAT16 in [
            *tensor_types,
            *initializer_types,
            *constant_types,
        ]

        expected_input = contract["input"]
        expected_outputs = get_output_contracts(contract)
        expected_onnx = contract["onnx"]
        if observed["inputs"] != [
            {
                "name": expected_input["name"],
                "dtype": expected_input["dtype"],
                "shape": expected_input["shape"],
            }
        ]:
            errors.append(f"input contract mismatch: {observed['inputs']}")
        expected_output_tensors = [
            {
                "name": output["name"],
                "dtype": output["dtype"],
                "shape": output["shape"],
            }
            for output in expected_outputs
        ]
        if observed["outputs"] != expected_output_tensors:
            errors.append(f"output contract mismatch: {observed['outputs']}")
        if observed["opsets"] != expected_onnx["opsets"]:
            errors.append(f"opset mismatch: {observed['opsets']}")
        for field in ("ir_version", "producer_name", "producer_version"):
            if field in expected_onnx and observed[field] != expected_onnx[field]:
                errors.append(
                    f"{field} expected={expected_onnx[field]} actual={observed[field]}"
                )
        if (
            "operator_counts" in expected_onnx
            and observed["operator_counts"] != expected_onnx["operator_counts"]
        ):
            errors.append(f"operator counts mismatch: {observed['operator_counts']}")
        expected_ultralytics = expected_onnx.get("ultralytics_version")
        if (
            expected_ultralytics is not None
            and observed["metadata"].get("version") != expected_ultralytics
        ):
            errors.append(
                "Ultralytics metadata version "
                f"expected={expected_ultralytics} "
                f"actual={observed['metadata'].get('version')}"
            )
        embedded_nms = observed["operator_counts"].get("NonMaxSuppression", 0) > 0
        observed["embedded_nms"] = embedded_nms
        expected_embedded_nms = {
            output["embedded_nms"]
            for output in expected_outputs
            if "embedded_nms" in output
        }
        if len(expected_embedded_nms) != 1:
            errors.append(
                "output contracts must declare one consistent embedded_nms value"
            )
        elif embedded_nms != next(iter(expected_embedded_nms)):
            errors.append(
                f"embedded NMS expected={next(iter(expected_embedded_nms))} "
                f"actual={embedded_nms}"
            )
        for field in ("dynamic_interface_axes", "fp16_present"):
            if field in expected_onnx and observed[field] != expected_onnx[field]:
                errors.append(
                    f"{field} expected={expected_onnx[field]} actual={observed[field]}"
                )

    if errors:
        raise ValueError("model contract verification failed: " + "; ".join(errors))
    return observed


def _tensor_value_info(value_info: Any) -> dict[str, Any]:
    tensor_type = value_info.type.tensor_type
    dtype_by_id = {1: "float32", 2: "uint8", 3: "int8"}
    dtype = dtype_by_id.get(tensor_type.elem_type, f"onnx_type_{tensor_type.elem_type}")
    shape: list[int | str] = []
    for dimension in tensor_type.shape.dim:
        if dimension.HasField("dim_value"):
            shape.append(dimension.dim_value)
        else:
            shape.append(dimension.dim_param)
    return {"name": value_info.name, "dtype": dtype, "shape": shape}
