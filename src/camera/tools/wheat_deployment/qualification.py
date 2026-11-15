"""Host-only FP32 qualification for guarded YOLOv8 predecode candidates."""

from __future__ import annotations

from collections import Counter
import hashlib
import io
from pathlib import Path
import platform
import sys
from typing import Any

import numpy as np

from .contract import sha256_file
from .head_transform import (
    SIX_HEAD_OUTPUT_NAMES,
    SIX_HEAD_OUTPUT_SPECS,
    derive_six_head_output_model,
)
from .manifest import write_manifest
from .model_transform import (
    QAT_FULL320_DFL_PROJECTION,
    derive_predecode_output_model,
    wrap_predecode_output_identities,
)
from .postprocess import (
    assemble_six_head_logits,
    decode_yolo11_output,
    global_nms,
    reconstruct_yolov8_output,
)


FP32_ABSOLUTE_TOLERANCE = 0.000125
CONFIDENCE_THRESHOLD = 0.25
IOU_THRESHOLD = 0.45
CANDIDATE_CAPACITY = 300
RESULT_CAPACITY = 64


def qualify_six_head_candidate(
    source_path: Path,
    golden_input_path: Path,
    output_dir: Path,
    *,
    expected_source_sha256: str,
) -> dict[str, Any]:
    """Derive and qualify the six final-Conv FP32 output candidate."""

    actual_source_sha256 = sha256_file(source_path)
    if actual_source_sha256 != expected_source_sha256.lower():
        raise ValueError(
            "immutable source hash mismatch: "
            f"expected={expected_source_sha256.lower()} actual={actual_source_sha256}"
        )
    candidate_path = (
        output_dir
        / "derived"
        / "yolov8n_wheat_full320_qat_six_head_outputs.onnx"
    )
    first_transform = derive_six_head_output_model(source_path, candidate_path)
    second_transform = derive_six_head_output_model(source_path, candidate_path)
    if first_transform != second_transform:
        raise ValueError("six-head transform is not deterministic")

    try:
        import onnx
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError(
            "onnx and onnxruntime are required for six-head qualification"
        ) from exc

    model_invariants = {
        "source": _inspect_model(source_path, onnx),
        "six_head": _inspect_model(candidate_path, onnx),
    }
    _require_six_head_candidate_invariants(model_invariants)

    session_options = ort.SessionOptions()
    session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    source_session = ort.InferenceSession(
        str(source_path),
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )
    candidate_session = ort.InferenceSession(
        str(candidate_path),
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )

    golden_input = np.load(golden_input_path, allow_pickle=False)
    _require_input_tensor(golden_input, "fixed_golden")
    shape = (1, 3, 320, 320)
    structured_input = (
        (np.arange(np.prod(shape), dtype=np.uint32) % 256)
        .astype(np.float32)
        .reshape(shape)
        / np.float32(255.0)
    )
    cases = (
        (
            "fixed_golden",
            golden_input,
            {
                "kind": "existing_project_golden",
                "source_path": golden_input_path.as_posix(),
                "source_file_sha256": sha256_file(golden_input_path),
            },
        ),
        (
            "synthetic_zero",
            np.zeros(shape, dtype=np.float32),
            {"kind": "deterministic_synthetic", "recipe": "np.zeros(float32)"},
        ),
        (
            "synthetic_u8_ramp",
            structured_input,
            {
                "kind": "deterministic_synthetic",
                "recipe": "float32((uint32 arange(CHW) % 256) / 255.0)",
            },
        ),
    )
    case_results = [
        _qualify_six_head_case(
            case_id,
            input_tensor,
            recipe,
            output_dir / "fp32" / case_id,
            source_session,
            candidate_session,
        )
        for case_id, input_tensor, recipe in cases
    ]

    report = {
        "schema_version": 1,
        "status": "fp32_static_qualified",
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": ort.__version__,
            "provider": "CPUExecutionProvider",
            "graph_optimization": "ORT_DISABLE_ALL",
        },
        "thresholds": {
            "confidence": CONFIDENCE_THRESHOLD,
            "iou": IOU_THRESHOLD,
            "candidate_capacity": CANDIDATE_CAPACITY,
            "result_capacity": RESULT_CAPACITY,
        },
        "tolerance": {"absolute": FP32_ABSOLUTE_TOLERANCE, "relative": 0.0},
        "dfl_projection": {
            "dtype": "float32",
            "shape": [16],
            "values": QAT_FULL320_DFL_PROJECTION.tolist(),
            "tensor_sha256": _tensor_sha256(QAT_FULL320_DFL_PROJECTION),
        },
        "output_assembly": {
            "output_order": list(SIX_HEAD_OUTPUT_NAMES),
            "flatten": "NCHW row-major to [1,C,H*W] per output",
            "scale_order": ["s8", "s16", "s32"],
            "materialize_fp32_concatenation_on_mcu": False,
        },
        "artifacts": {
            "source": _artifact_record(source_path),
            "six_head": _artifact_record(candidate_path),
        },
        "transform": {
            **first_transform,
            "deterministic_rerun_equal": True,
        },
        "model_invariants": model_invariants,
        "cases": case_results,
        "parity": {
            "case_count": len(case_results),
            "deterministic_synthetic_count": 2,
            "every_source_reconstruction_within_tolerance": True,
            "every_candidate_index_sequence_equal": True,
            "every_candidate_count_equal": True,
            "every_nms_index_sequence_equal": True,
            "every_nms_count_equal": True,
        },
        "ruhmi": {
            "quantizer_constructor_preflight_status": "pending_parent_owned",
            "quantization_compile_status": "blocked_until_preflight_passes",
            "invocations_performed_by_this_command": 0,
        },
    }
    write_manifest(report, output_dir / "fp32_qualification.json")
    return report


def qualify_predecode_candidate(
    source_path: Path,
    golden_input_path: Path,
    output_dir: Path,
    *,
    expected_source_sha256: str,
) -> dict[str, Any]:
    """Derive, inspect, and qualify direct and identity-wrapped FP32 graphs."""

    actual_source_sha256 = sha256_file(source_path)
    if actual_source_sha256 != expected_source_sha256.lower():
        raise ValueError(
            "immutable source hash mismatch: "
            f"expected={expected_source_sha256.lower()} actual={actual_source_sha256}"
        )
    direct_path = (
        output_dir / "derived" / "yolov8n_wheat_full320_qat_predecode_outputs.onnx"
    )
    wrapped_path = (
        output_dir
        / "derived"
        / "yolov8n_wheat_full320_qat_predecode_identity_outputs.onnx"
    )
    first_transform = derive_predecode_output_model(source_path, direct_path)
    second_transform = derive_predecode_output_model(source_path, direct_path)
    first_wrapper = wrap_predecode_output_identities(direct_path, wrapped_path)
    second_wrapper = wrap_predecode_output_identities(direct_path, wrapped_path)
    if first_transform != second_transform or first_wrapper != second_wrapper:
        raise ValueError("predecode transform is not deterministic")

    try:
        import onnx
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError(
            "onnx and onnxruntime are required for predecode qualification"
        ) from exc

    model_invariants = {
        "source": _inspect_model(source_path, onnx),
        "direct": _inspect_model(direct_path, onnx),
        "identity_wrapped": _inspect_model(wrapped_path, onnx),
    }
    _require_candidate_invariants(model_invariants)

    session_options = ort.SessionOptions()
    session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    source_session = ort.InferenceSession(
        str(source_path),
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )
    direct_session = ort.InferenceSession(
        str(direct_path),
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )
    wrapped_session = ort.InferenceSession(
        str(wrapped_path),
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )

    golden_input = np.load(golden_input_path, allow_pickle=False)
    _require_input_tensor(golden_input, "fixed_golden")
    shape = (1, 3, 320, 320)
    structured_input = (
        (np.arange(np.prod(shape), dtype=np.uint32) % 256)
        .astype(np.float32)
        .reshape(shape)
        / np.float32(255.0)
    )
    cases = (
        (
            "fixed_golden",
            golden_input,
            {
                "kind": "existing_project_golden",
                "source_path": golden_input_path.as_posix(),
                "source_file_sha256": sha256_file(golden_input_path),
            },
        ),
        (
            "synthetic_zero",
            np.zeros(shape, dtype=np.float32),
            {"kind": "deterministic_synthetic", "recipe": "np.zeros(float32)"},
        ),
        (
            "synthetic_u8_ramp",
            structured_input,
            {
                "kind": "deterministic_synthetic",
                "recipe": "float32((uint32 arange(CHW) % 256) / 255.0)",
            },
        ),
    )
    case_results = [
        _qualify_case(
            case_id,
            input_tensor,
            recipe,
            output_dir / "fp32" / case_id,
            source_session,
            direct_session,
            wrapped_session,
        )
        for case_id, input_tensor, recipe in cases
    ]

    report = {
        "schema_version": 1,
        "status": "fp32_static_qualified",
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": ort.__version__,
            "provider": "CPUExecutionProvider",
            "graph_optimization": "ORT_DISABLE_ALL",
        },
        "thresholds": {
            "confidence": CONFIDENCE_THRESHOLD,
            "iou": IOU_THRESHOLD,
            "candidate_capacity": CANDIDATE_CAPACITY,
            "result_capacity": RESULT_CAPACITY,
        },
        "tolerance": {"absolute": FP32_ABSOLUTE_TOLERANCE, "relative": 0.0},
        "dfl_projection": {
            "dtype": "float32",
            "shape": [16],
            "values": QAT_FULL320_DFL_PROJECTION.tolist(),
            "tensor_sha256": _tensor_sha256(QAT_FULL320_DFL_PROJECTION),
        },
        "artifacts": {
            "source": _artifact_record(source_path),
            "direct": _artifact_record(direct_path),
            "identity_wrapped": _artifact_record(wrapped_path),
        },
        "transform": {
            **first_transform,
            "deterministic_rerun_equal": True,
            "identity_wrapper": {
                **first_wrapper,
                "deterministic_rerun_equal": True,
            },
        },
        "model_invariants": model_invariants,
        "cases": case_results,
        "parity": {
            "case_count": len(case_results),
            "deterministic_synthetic_count": 2,
            "every_direct_identity_output_bitwise_equal": True,
            "every_source_reconstruction_within_tolerance": True,
            "every_candidate_index_sequence_equal": True,
            "every_candidate_count_equal": True,
            "every_nms_index_sequence_equal": True,
            "every_nms_count_equal": True,
        },
        "ruhmi": {
            "frontend_preflight_status": "pending_parent_owned",
            "quantization_compile_status": "pending_exactly_one_pinned_gate",
            "invocations_performed_by_this_command": 0,
        },
    }
    write_manifest(report, output_dir / "fp32_qualification.json")
    return report


def _qualify_case(
    case_id: str,
    input_tensor: np.ndarray,
    recipe: dict[str, Any],
    case_dir: Path,
    source_session: Any,
    direct_session: Any,
    wrapped_session: Any,
) -> dict[str, Any]:
    _require_input_tensor(input_tensor, case_id)
    source_output = source_session.run(["output0"], {"images": input_tensor})[0]
    direct_outputs = direct_session.run(
        ["regression_logits", "class_logits"], {"images": input_tensor}
    )
    wrapped_outputs = wrapped_session.run(
        ["regression_logits", "class_logits"], {"images": input_tensor}
    )
    if not all(np.all(np.isfinite(value)) for value in [source_output, *direct_outputs]):
        raise ValueError(f"{case_id}: source or direct output contains non-finite values")
    direct_identity_equal = all(
        np.array_equal(direct, wrapped)
        for direct, wrapped in zip(direct_outputs, wrapped_outputs)
    )
    if not direct_identity_equal:
        raise ValueError(f"{case_id}: identity wrapper changed a model output")

    reconstructed = reconstruct_yolov8_output(
        direct_outputs[0],
        direct_outputs[1],
        QAT_FULL320_DFL_PROJECTION,
    )
    difference = np.abs(source_output - reconstructed)
    maximum_difference = float(np.max(difference))
    if not np.allclose(
        source_output,
        reconstructed,
        atol=FP32_ABSOLUTE_TOLERANCE,
        rtol=0.0,
    ):
        raise ValueError(
            f"{case_id}: reconstruction exceeds absolute tolerance: "
            f"maximum={maximum_difference} tolerance={FP32_ABSOLUTE_TOLERANCE}"
        )

    source_candidates, source_metrics = decode_yolo11_output(
        source_output,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    reconstructed_candidates, reconstructed_metrics = decode_yolo11_output(
        reconstructed,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    source_nms = global_nms(
        source_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=RESULT_CAPACITY,
    )
    reconstructed_nms = global_nms(
        reconstructed_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=RESULT_CAPACITY,
    )
    source_candidate_indices = [item.candidate_index for item in source_candidates]
    reconstructed_candidate_indices = [
        item.candidate_index for item in reconstructed_candidates
    ]
    source_nms_indices = [item.candidate_index for item in source_nms]
    reconstructed_nms_indices = [item.candidate_index for item in reconstructed_nms]
    if source_metrics != reconstructed_metrics:
        raise ValueError(f"{case_id}: source/reconstructed candidate metrics differ")
    if source_candidate_indices != reconstructed_candidate_indices:
        raise ValueError(f"{case_id}: retained candidate indices differ")
    if source_nms_indices != reconstructed_nms_indices:
        raise ValueError(f"{case_id}: NMS retained indices differ")

    paths = {
        "input": case_dir / "input_fp32.npy",
        "source_output": case_dir / "source_output_fp32.npy",
        "regression_logits": case_dir / "regression_logits_fp32.npy",
        "class_logits": case_dir / "class_logits_fp32.npy",
        "reconstructed_output": case_dir / "reconstructed_output_fp32.npy",
    }
    values = {
        "input": input_tensor,
        "source_output": source_output,
        "regression_logits": direct_outputs[0],
        "class_logits": direct_outputs[1],
        "reconstructed_output": reconstructed,
    }
    for key, path in paths.items():
        _write_npy(path, values[key])

    return {
        "case_id": case_id,
        "input": {
            **recipe,
            "shape": list(input_tensor.shape),
            "dtype": str(input_tensor.dtype),
            "tensor_sha256": _tensor_sha256(input_tensor),
            "evidence_file": paths["input"].relative_to(case_dir.parents[1]).as_posix(),
            "evidence_file_sha256": sha256_file(paths["input"]),
        },
        "outputs": {
            key: {
                "shape": list(values[key].shape),
                "dtype": str(values[key].dtype),
                "tensor_sha256": _tensor_sha256(values[key]),
                "evidence_file": path.relative_to(case_dir.parents[1]).as_posix(),
                "evidence_file_sha256": sha256_file(path),
            }
            for key, path in paths.items()
            if key != "input"
        },
        "direct_identity_outputs_bitwise_equal": direct_identity_equal,
        "maximum_absolute_difference": maximum_difference,
        "mean_absolute_difference": float(np.mean(difference)),
        "candidate_metrics": source_metrics,
        "retained_candidate_count": len(source_candidate_indices),
        "retained_candidate_indices_sha256": _index_sha256(source_candidate_indices),
        "retained_candidate_indices_equal": True,
        "nms_count": len(source_nms_indices),
        "nms_indices_sha256": _index_sha256(source_nms_indices),
        "nms_indices_equal": True,
        "score_max": float(np.max(source_output[:, 4, :])),
    }


def _qualify_six_head_case(
    case_id: str,
    input_tensor: np.ndarray,
    recipe: dict[str, Any],
    case_dir: Path,
    source_session: Any,
    candidate_session: Any,
) -> dict[str, Any]:
    _require_input_tensor(input_tensor, case_id)
    source_output = source_session.run(["output0"], {"images": input_tensor})[0]
    head_outputs = candidate_session.run(
        list(SIX_HEAD_OUTPUT_NAMES),
        {"images": input_tensor},
    )
    if not all(np.all(np.isfinite(value)) for value in [source_output, *head_outputs]):
        raise ValueError(f"{case_id}: source or six-head output is non-finite")

    regression_logits, class_logits = assemble_six_head_logits(head_outputs)
    reconstructed = reconstruct_yolov8_output(
        regression_logits,
        class_logits,
        QAT_FULL320_DFL_PROJECTION,
    )
    difference = np.abs(source_output - reconstructed)
    maximum_difference = float(np.max(difference))
    if not np.allclose(
        source_output,
        reconstructed,
        atol=FP32_ABSOLUTE_TOLERANCE,
        rtol=0.0,
    ):
        raise ValueError(
            f"{case_id}: six-head reconstruction exceeds absolute tolerance: "
            f"maximum={maximum_difference} tolerance={FP32_ABSOLUTE_TOLERANCE}"
        )

    source_candidates, source_metrics = decode_yolo11_output(
        source_output,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    reconstructed_candidates, reconstructed_metrics = decode_yolo11_output(
        reconstructed,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    source_nms = global_nms(
        source_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=RESULT_CAPACITY,
    )
    reconstructed_nms = global_nms(
        reconstructed_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=RESULT_CAPACITY,
    )
    source_candidate_indices = [item.candidate_index for item in source_candidates]
    reconstructed_candidate_indices = [
        item.candidate_index for item in reconstructed_candidates
    ]
    source_nms_indices = [item.candidate_index for item in source_nms]
    reconstructed_nms_indices = [
        item.candidate_index for item in reconstructed_nms
    ]
    if source_metrics != reconstructed_metrics:
        raise ValueError(f"{case_id}: source/reconstructed candidate metrics differ")
    if source_candidate_indices != reconstructed_candidate_indices:
        raise ValueError(f"{case_id}: retained candidate indices differ")
    if source_nms_indices != reconstructed_nms_indices:
        raise ValueError(f"{case_id}: NMS retained indices differ")

    paths = {
        "input": case_dir / "input_fp32.npy",
        "source_output": case_dir / "source_output_fp32.npy",
        **{
            name: case_dir / f"{name}_fp32.npy"
            for name in SIX_HEAD_OUTPUT_NAMES
        },
        "regression_logits": case_dir / "regression_logits_fp32.npy",
        "class_logits": case_dir / "class_logits_fp32.npy",
        "reconstructed_output": case_dir / "reconstructed_output_fp32.npy",
    }
    values = {
        "input": input_tensor,
        "source_output": source_output,
        **dict(zip(SIX_HEAD_OUTPUT_NAMES, head_outputs)),
        "regression_logits": regression_logits,
        "class_logits": class_logits,
        "reconstructed_output": reconstructed,
    }
    for key, path in paths.items():
        _write_npy(path, values[key])

    return {
        "case_id": case_id,
        "input": {
            **recipe,
            "shape": list(input_tensor.shape),
            "dtype": str(input_tensor.dtype),
            "tensor_sha256": _tensor_sha256(input_tensor),
            "evidence_file": paths["input"].relative_to(case_dir.parents[1]).as_posix(),
            "evidence_file_sha256": sha256_file(paths["input"]),
        },
        "outputs": {
            key: {
                "shape": list(values[key].shape),
                "dtype": str(values[key].dtype),
                "minimum": float(np.min(values[key])),
                "maximum": float(np.max(values[key])),
                "tensor_sha256": _tensor_sha256(values[key]),
                "evidence_file": path.relative_to(case_dir.parents[1]).as_posix(),
                "evidence_file_sha256": sha256_file(path),
            }
            for key, path in paths.items()
            if key != "input"
        },
        "maximum_absolute_difference": maximum_difference,
        "mean_absolute_difference": float(np.mean(difference)),
        "candidate_metrics": source_metrics,
        "retained_candidate_count": len(source_candidate_indices),
        "retained_candidate_indices_sha256": _index_sha256(source_candidate_indices),
        "retained_candidate_indices_equal": True,
        "nms_count": len(source_nms_indices),
        "nms_indices_sha256": _index_sha256(source_nms_indices),
        "nms_indices_equal": True,
        "score_max": float(np.max(source_output[:, 4, :])),
    }


def _inspect_model(path: Path, onnx: Any) -> dict[str, Any]:
    model = onnx.load(str(path), load_external_data=False)
    onnx.checker.check_model(model)
    inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    tensor_types = [
        item.type.tensor_type.elem_type
        for item in [
            *inferred.graph.input,
            *inferred.graph.output,
            *inferred.graph.value_info,
        ]
    ]
    initializer_types = [item.data_type for item in model.graph.initializer]
    constant_types = [
        attribute.t.data_type
        for node in model.graph.node
        for attribute in node.attribute
        if attribute.type == onnx.AttributeProto.TENSOR
    ]
    outputs = [_tensor_contract(item, onnx) for item in inferred.graph.output]
    output_producers = {
        output["name"]: [
            {"name": node.name, "op_type": node.op_type}
            for node in model.graph.node
            if output["name"] in node.output
        ]
        for output in outputs
    }
    return {
        **_artifact_record(path),
        "checker_passed": True,
        "strict_shape_inference_passed": True,
        "input": [_tensor_contract(item, onnx) for item in inferred.graph.input],
        "outputs": outputs,
        "output_producers": output_producers,
        "dynamic_interface_axes": any(
            not dimension.HasField("dim_value")
            for item in [*inferred.graph.input, *inferred.graph.output]
            for dimension in item.type.tensor_type.shape.dim
        ),
        "fp16_present": onnx.TensorProto.FLOAT16
        in [*tensor_types, *initializer_types, *constant_types],
        "embedded_nms": any(
            node.op_type == "NonMaxSuppression" for node in model.graph.node
        ),
        "node_count": len(model.graph.node),
        "initializer_count": len(model.graph.initializer),
        "operator_counts": dict(
            sorted(Counter(node.op_type for node in model.graph.node).items())
        ),
        "opsets": {
            item.domain or "ai.onnx": item.version for item in model.opset_import
        },
        "ir_version": model.ir_version,
        "producer_name": model.producer_name,
        "producer_version": model.producer_version,
    }


def _require_candidate_invariants(models: dict[str, dict[str, Any]]) -> None:
    expected_source_outputs = [
        {"name": "output0", "dtype": "float32", "shape": [1, 5, 2100]}
    ]
    expected_candidate_outputs = [
        {
            "name": "regression_logits",
            "dtype": "float32",
            "shape": [1, 64, 2100],
        },
        {"name": "class_logits", "dtype": "float32", "shape": [1, 1, 2100]},
    ]
    expected_input = [
        {"name": "images", "dtype": "float32", "shape": [1, 3, 320, 320]}
    ]
    if models["source"]["outputs"] != expected_source_outputs:
        raise ValueError("source output contract changed")
    for name, record in models.items():
        if record["input"] != expected_input:
            raise ValueError(f"{name}: input contract changed")
        if record["dynamic_interface_axes"]:
            raise ValueError(f"{name}: dynamic interface axis present")
        if record["fp16_present"]:
            raise ValueError(f"{name}: FP16 tensor present")
        if record["embedded_nms"]:
            raise ValueError(f"{name}: embedded NMS present")
    for name in ("direct", "identity_wrapped"):
        if models[name]["outputs"] != expected_candidate_outputs:
            raise ValueError(f"{name}: predecode output contract changed")
    direct_producers = models["direct"]["output_producers"]
    if any(
        producers != [{"name": "/model.22/Split", "op_type": "Split"}]
        for producers in direct_producers.values()
    ):
        raise ValueError("direct predecode outputs must be produced by /model.22/Split")
    wrapped_producers = models["identity_wrapped"]["output_producers"]
    if any(
        len(producers) != 1 or producers[0]["op_type"] != "Identity"
        for producers in wrapped_producers.values()
    ):
        raise ValueError("wrapped predecode outputs must have unique Identity producers")


def _require_six_head_candidate_invariants(
    models: dict[str, dict[str, Any]],
) -> None:
    expected_input = [
        {"name": "images", "dtype": "float32", "shape": [1, 3, 320, 320]}
    ]
    expected_source_outputs = [
        {"name": "output0", "dtype": "float32", "shape": [1, 5, 2100]}
    ]
    expected_candidate_outputs = [
        {
            "name": item["name"],
            "dtype": "float32",
            "shape": item["shape"],
        }
        for item in SIX_HEAD_OUTPUT_SPECS
    ]
    if models["source"]["outputs"] != expected_source_outputs:
        raise ValueError("source output contract changed")
    if models["six_head"]["outputs"] != expected_candidate_outputs:
        raise ValueError("six-head output contract changed")

    for name, record in models.items():
        if record["input"] != expected_input:
            raise ValueError(f"{name}: input contract changed")
        if record["dynamic_interface_axes"]:
            raise ValueError(f"{name}: dynamic interface axis present")
        if record["fp16_present"]:
            raise ValueError(f"{name}: FP16 tensor present")
        if record["embedded_nms"]:
            raise ValueError(f"{name}: embedded NMS present")

    candidate_producers = models["six_head"]["output_producers"]
    for spec in SIX_HEAD_OUTPUT_SPECS:
        if candidate_producers[spec["name"]] != [
            {"name": spec["node_name"], "op_type": "Conv"}
        ]:
            raise ValueError(
                f"{spec['name']}: output must be produced directly by frozen Conv"
            )


def _tensor_contract(value_info: Any, onnx: Any) -> dict[str, Any]:
    tensor_type = value_info.type.tensor_type
    dtype_by_id = {
        onnx.TensorProto.FLOAT: "float32",
        onnx.TensorProto.UINT8: "uint8",
        onnx.TensorProto.INT8: "int8",
    }
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


def _require_input_tensor(values: np.ndarray, case_id: str) -> None:
    if values.shape != (1, 3, 320, 320) or values.dtype != np.float32:
        raise ValueError(
            f"{case_id}: input must be float32 [1,3,320,320], "
            f"got dtype={values.dtype} shape={values.shape}"
        )
    if not np.all(np.isfinite(values)):
        raise ValueError(f"{case_id}: input contains non-finite values")


def _write_npy(path: Path, values: np.ndarray) -> None:
    stream = io.BytesIO()
    np.save(stream, values, allow_pickle=False)
    payload = stream.getvalue()
    if path.exists() and path.read_bytes() != payload:
        raise FileExistsError(f"refusing to replace non-matching evidence: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        path.write_bytes(payload)


def _artifact_record(path: Path) -> dict[str, Any]:
    return {"path": path.as_posix(), "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def _tensor_sha256(values: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(values).tobytes()).hexdigest()


def _index_sha256(indices: list[int]) -> str:
    return hashlib.sha256(np.asarray(indices, dtype=np.int32).tobytes()).hexdigest()
