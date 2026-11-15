"""PC numerical gate for the generated YOLOX tile224 NPU partition."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Callable
import zlib

import numpy as np


SOURCE_MODEL_SHA256 = (
    "7f6a058eb5b4cc4cc61942cdac55a6645fbffe9992731f1f6d72d897338f07f3"
)
PRE_VELA_MODEL_SHA256 = (
    "c13c1477365d6fbbfb35eaa8feba3de26caf611db5b0c2004b504f4882092117"
)
SANITY_OUTPUT_CRC32 = 0x6ACE6AFF
SOURCE_INPUT_SHAPE = (1, 224, 224, 3)
FOCUS_INPUT_SHAPE = (1, 112, 112, 3)
OUTPUT_SHAPE = (1, 1029, 6)
INPUT_QUANTIZATION = (1.0, -128)
OUTPUT_QUANTIZATION = (0.03774220868945122, -48)
FOCUS_OFFSETS = {
    "model_3_tf_strided_slice_1_StridedSlice3_70180": (0, 1),
    "model_3_tf_strided_slice_2_StridedSlice1_70181": (1, 0),
    "model_3_tf_strided_slice_3_StridedSlice1_70182": (1, 1),
    "model_3_tf_strided_slice_StridedSlice1_70183": (0, 0),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_focus_inputs(source_input: np.ndarray) -> dict[str, np.ndarray]:
    """Reproduce the four generated CPU StridedSlice outputs by tensor name."""

    if source_input.shape != SOURCE_INPUT_SHAPE or source_input.dtype != np.int8:
        raise ValueError(
            "source input must be int8 NHWC [1,224,224,3], got "
            f"{source_input.dtype} {list(source_input.shape)}"
        )
    return {
        name: np.ascontiguousarray(source_input[:, row::2, column::2, :])
        for name, (row, column) in FOCUS_OFFSETS.items()
    }


def summarize_int8(values: np.ndarray) -> dict[str, Any]:
    raw = np.ascontiguousarray(values).tobytes()
    unique = np.unique(values)
    return {
        "shape": list(values.shape),
        "dtype": str(values.dtype),
        "bytes": len(raw),
        "crc32": f"0x{zlib.crc32(raw) & 0xFFFFFFFF:08X}",
        "sha256": hashlib.sha256(raw).hexdigest(),
        "minimum": int(values.min()),
        "maximum": int(values.max()),
        "unique_values": int(unique.size),
        "sum": int(values.astype(np.int64).sum()),
    }


def compare_int8_outputs(actual: np.ndarray, expected: np.ndarray) -> dict[str, Any]:
    if actual.shape != expected.shape:
        return {
            "exact": False,
            "changed_values": None,
            "maximum_absolute_error": None,
            "failure": (
                f"shape mismatch: actual={list(actual.shape)} "
                f"expected={list(expected.shape)}"
            ),
        }
    difference = np.abs(actual.astype(np.int16) - expected.astype(np.int16))
    changed = int(np.count_nonzero(difference))
    maximum = int(difference.max(initial=0))
    return {
        "exact": changed == 0,
        "changed_values": changed,
        "maximum_absolute_error": maximum,
        "failure": None if changed == 0 else "source and pre-Vela outputs differ",
    }


def run_numeric_gate(
    source_model: Path,
    pre_vela_model: Path,
    *,
    interpreter_factory: Callable[..., Any],
    expected_source_sha256: str = SOURCE_MODEL_SHA256,
    expected_pre_vela_sha256: str = PRE_VELA_MODEL_SHA256,
) -> dict[str, Any]:
    """Run the frozen all-minus-128 gate and a Focus-order diagnostic case."""

    source_model = source_model.resolve()
    pre_vela_model = pre_vela_model.resolve()
    failures: list[str] = []
    for label, path in (("source", source_model), ("pre-Vela", pre_vela_model)):
        if not path.is_file():
            failures.append(f"{label} model is missing: {path}")
    if failures:
        return _rejected_report(source_model, pre_vela_model, failures)

    source_hash = sha256_file(source_model)
    pre_vela_hash = sha256_file(pre_vela_model)
    if source_hash != expected_source_sha256.lower():
        failures.append(
            "source model hash mismatch: "
            f"expected={expected_source_sha256.lower()} actual={source_hash}"
        )
    if pre_vela_hash != expected_pre_vela_sha256.lower():
        failures.append(
            "pre-Vela model hash mismatch: "
            f"expected={expected_pre_vela_sha256.lower()} actual={pre_vela_hash}"
        )
    if failures:
        return _rejected_report(source_model, pre_vela_model, failures)

    source = interpreter_factory(model_path=str(source_model))
    pre_vela = interpreter_factory(model_path=str(pre_vela_model))
    source.allocate_tensors()
    pre_vela.allocate_tensors()
    source_inputs = source.get_input_details()
    source_outputs = source.get_output_details()
    pre_vela_inputs = pre_vela.get_input_details()
    pre_vela_outputs = pre_vela.get_output_details()

    failures.extend(_validate_source_abi(source_inputs, source_outputs))
    failures.extend(_validate_pre_vela_abi(pre_vela_inputs, pre_vela_outputs))
    if failures:
        return _rejected_report(source_model, pre_vela_model, failures)

    cases = {
        "all_minus_128": np.full(SOURCE_INPUT_SHAPE, -128, dtype=np.int8),
        "deterministic_focus_pattern": (
            (np.arange(np.prod(SOURCE_INPUT_SHAPE), dtype=np.int64) % 256) - 128
        ).astype(np.int8).reshape(SOURCE_INPUT_SHAPE),
    }
    case_reports: dict[str, Any] = {}
    for case_name, source_input in cases.items():
        source_output = _invoke_source(
            source, source_inputs[0], source_outputs[0], source_input
        )
        pre_vela_output = _invoke_pre_vela(
            pre_vela, pre_vela_inputs, pre_vela_outputs[0], source_input
        )
        comparison = compare_int8_outputs(pre_vela_output, source_output)
        source_summary = summarize_int8(source_output)
        pre_vela_summary = summarize_int8(pre_vela_output)
        if not comparison["exact"]:
            failures.append(f"{case_name}: {comparison['failure']}")
        if case_name == "all_minus_128":
            expected_crc = f"0x{SANITY_OUTPUT_CRC32:08X}"
            if source_summary["crc32"] != expected_crc:
                failures.append(
                    f"all_minus_128: source CRC mismatch: expected={expected_crc} "
                    f"actual={source_summary['crc32']}"
                )
            if pre_vela_summary["crc32"] != expected_crc:
                failures.append(
                    f"all_minus_128: pre-Vela CRC mismatch: expected={expected_crc} "
                    f"actual={pre_vela_summary['crc32']}"
                )
        case_reports[case_name] = {
            "input": summarize_int8(source_input),
            "source_output": source_summary,
            "pre_vela_output": pre_vela_summary,
            "comparison": comparison,
        }

    return {
        "schema_version": 1,
        "status": "passed" if not failures else "failed",
        "scope": "source-tflite-and-generated-pre-vela-numeric-parity",
        "board_numeric_gate_proven": False,
        "expected_board_sanity_crc32": f"0x{SANITY_OUTPUT_CRC32:08X}",
        "artifacts": {
            "source_model": _artifact_record(source_model, source_hash),
            "pre_vela_model": _artifact_record(pre_vela_model, pre_vela_hash),
        },
        "source_abi": {
            "inputs": [_detail_summary(item) for item in source_inputs],
            "outputs": [_detail_summary(item) for item in source_outputs],
        },
        "pre_vela_abi": {
            "inputs": [_detail_summary(item) for item in pre_vela_inputs],
            "outputs": [_detail_summary(item) for item in pre_vela_outputs],
            "focus_offsets_by_input_name": {
                name: [row, column]
                for name, (row, column) in FOCUS_OFFSETS.items()
            },
        },
        "cases": case_reports,
        "limitations": [
            "The host gate stops before Vela's ethos-u custom operator.",
            "It does not execute the command stream, Ethos-U driver, or board memory.",
            "The all-minus-128 case alone cannot distinguish Focus input order; the "
            "deterministic pattern case covers that mapping.",
        ],
        "failures": failures,
    }


def _invoke_source(
    interpreter: Any,
    input_detail: dict[str, Any],
    output_detail: dict[str, Any],
    source_input: np.ndarray,
) -> np.ndarray:
    interpreter.set_tensor(input_detail["index"], source_input)
    interpreter.invoke()
    return np.asarray(interpreter.get_tensor(output_detail["index"])).copy()


def _invoke_pre_vela(
    interpreter: Any,
    input_details: list[dict[str, Any]],
    output_detail: dict[str, Any],
    source_input: np.ndarray,
) -> np.ndarray:
    focus_inputs = build_focus_inputs(source_input)
    for detail in input_details:
        interpreter.set_tensor(detail["index"], focus_inputs[detail["name"]])
    interpreter.invoke()
    return np.asarray(interpreter.get_tensor(output_detail["index"])).copy()


def _validate_source_abi(
    inputs: list[dict[str, Any]], outputs: list[dict[str, Any]]
) -> list[str]:
    failures: list[str] = []
    if len(inputs) != 1:
        failures.append(f"source model must have one input, found {len(inputs)}")
    if len(outputs) != 1:
        failures.append(f"source model must have one output, found {len(outputs)}")
    if len(inputs) == 1:
        failures.extend(
            _validate_detail(
                "source input", inputs[0], SOURCE_INPUT_SHAPE, INPUT_QUANTIZATION
            )
        )
    if len(outputs) == 1:
        failures.extend(
            _validate_detail(
                "source output", outputs[0], OUTPUT_SHAPE, OUTPUT_QUANTIZATION
            )
        )
    return failures


def _validate_pre_vela_abi(
    inputs: list[dict[str, Any]], outputs: list[dict[str, Any]]
) -> list[str]:
    failures: list[str] = []
    actual_names = {str(item.get("name")) for item in inputs}
    if actual_names != set(FOCUS_OFFSETS):
        failures.append(
            "pre-Vela input names mismatch: "
            f"expected={sorted(FOCUS_OFFSETS)} actual={sorted(actual_names)}"
        )
    for detail in inputs:
        failures.extend(
            _validate_detail(
                f"pre-Vela input {detail.get('name')}",
                detail,
                FOCUS_INPUT_SHAPE,
                INPUT_QUANTIZATION,
            )
        )
    if len(outputs) != 1:
        failures.append(f"pre-Vela model must have one output, found {len(outputs)}")
    elif len(outputs) == 1:
        failures.extend(
            _validate_detail(
                "pre-Vela output", outputs[0], OUTPUT_SHAPE, OUTPUT_QUANTIZATION
            )
        )
    return failures


def _validate_detail(
    label: str,
    detail: dict[str, Any],
    expected_shape: tuple[int, ...],
    expected_quantization: tuple[float, int],
) -> list[str]:
    failures: list[str] = []
    shape = tuple(int(value) for value in detail.get("shape", ()))
    if shape != expected_shape:
        failures.append(
            f"{label} shape mismatch: expected={list(expected_shape)} "
            f"actual={list(shape)}"
        )
    dtype = np.dtype(detail.get("dtype")).name
    if dtype != "int8":
        failures.append(f"{label} dtype mismatch: expected=int8 actual={dtype}")
    quantization = detail.get("quantization")
    if not isinstance(quantization, (tuple, list)) or len(quantization) != 2:
        failures.append(f"{label} quantization is invalid: {quantization!r}")
    elif (
        float(quantization[0]) != expected_quantization[0]
        or int(quantization[1]) != expected_quantization[1]
    ):
        failures.append(
            f"{label} quantization mismatch: expected={expected_quantization!r} "
            f"actual={quantization!r}"
        )
    return failures


def _detail_summary(detail: dict[str, Any]) -> dict[str, Any]:
    quantization = detail.get("quantization")
    return {
        "name": detail.get("name"),
        "shape": [int(value) for value in detail.get("shape", ())],
        "dtype": np.dtype(detail.get("dtype")).name,
        "quantization": list(quantization) if quantization is not None else None,
    }


def _artifact_record(path: Path, sha256: str) -> dict[str, Any]:
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256,
    }


def _rejected_report(
    source_model: Path, pre_vela_model: Path, failures: list[str]
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "status": "failed",
        "scope": "source-tflite-and-generated-pre-vela-numeric-parity",
        "board_numeric_gate_proven": False,
        "artifacts": {
            "source_model": str(source_model),
            "pre_vela_model": str(pre_vela_model),
        },
        "failures": failures,
    }
