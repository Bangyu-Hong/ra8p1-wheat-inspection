#!/usr/bin/env python3
"""Validate a saved six-head MERA model on the fixed golden tensor."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import sys
import time
import traceback

import numpy as np

from wheat_deployment.head_transform import SIX_HEAD_OUTPUT_NAMES
from wheat_deployment.model_transform import QAT_FULL320_DFL_PROJECTION
from wheat_deployment.postprocess import (
    assemble_six_head_logits,
    decode_yolo11_output,
    global_nms,
    reconstruct_yolov8_output,
)


CONFIDENCE_THRESHOLD = 0.25
IOU_THRESHOLD = 0.45
CANDIDATE_CAPACITY = 300
FIRMWARE_RESULT_CAPACITY = 64


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _tensor_sha256(values: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(values).tobytes()).hexdigest()


def _write_npy(path: Path, values: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.save(path, values, allow_pickle=False)


def _output_record(path: Path, values: np.ndarray) -> dict[str, object]:
    return {
        "shape": list(values.shape),
        "dtype": str(values.dtype),
        "minimum": float(np.min(values)),
        "maximum": float(np.max(values)),
        "dynamic_range": float(np.max(values) - np.min(values)),
        "finite": bool(np.all(np.isfinite(values))),
        "tensor_sha256": _tensor_sha256(values),
        "evidence_file": path.as_posix(),
        "evidence_file_sha256": _sha256_file(path),
    }


def _match_outputs(
    outputs: dict[str, np.ndarray],
) -> tuple[list[np.ndarray], dict[str, str]]:
    ordered: list[np.ndarray] = []
    mapping: dict[str, str] = {}
    for logical_name in SIX_HEAD_OUTPUT_NAMES:
        matches = [
            name
            for name in outputs
            if name == logical_name or name.startswith(f"{logical_name}_")
        ]
        if len(matches) != 1:
            raise ValueError(
                f"expected one saved output for {logical_name}, found {matches}"
            )
        mapping[logical_name] = matches[0]
        ordered.append(outputs[matches[0]])
    if len(outputs) != len(ordered):
        raise ValueError(f"saved model exposed unexpected outputs: {sorted(outputs)}")
    return ordered, mapping


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fresh-interpreter saved MERA six-head acceptance gate"
    )
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--expected-model-sha256", required=True)
    parser.add_argument("--golden-input", required=True, type=Path)
    parser.add_argument("--fp32-source-output", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    model_path = args.model.resolve()
    golden_path = args.golden_input.resolve()
    source_output_path = args.fp32_source_output.resolve()
    output_dir = args.output_dir.resolve()
    for path in (model_path, golden_path, source_output_path):
        if not path.is_file():
            parser.error(f"required artifact does not exist: {path}")
    model_sha256 = _sha256_file(model_path)
    if model_sha256 != args.expected_model_sha256.lower():
        parser.error(
            "saved model hash mismatch: "
            f"expected={args.expected_model_sha256.lower()} actual={model_sha256}"
        )
    if output_dir.exists():
        parser.error(f"output directory already exists: {output_dir}")

    golden_input = np.load(golden_path, allow_pickle=False)
    fp32_source_output = np.load(source_output_path, allow_pickle=False)
    if golden_input.shape != (1, 3, 320, 320) or golden_input.dtype != np.float32:
        parser.error("golden input must be float32 [1,3,320,320]")
    if (
        fp32_source_output.shape != (1, 5, 2100)
        or fp32_source_output.dtype != np.float32
    ):
        parser.error("FP32 source output must be float32 [1,5,2100]")

    try:
        import mera
        from mera import Platform, Target
    except ImportError as exc:
        parser.error(f"pinned MERA environment is not active: {exc}")

    try:
        deployment_dir = output_dir / "deployment"
        started = time.perf_counter()
        with mera.Deployer(str(deployment_dir), overwrite=False) as deployer:
            model = mera.ModelLoader(deployer).from_quantized_mera(str(model_path))
            deployment = deployer.deploy(
                model,
                mera_platform=Platform.MCU_CPU,
                target=Target.MERAInterpreter,
            )
            runner = deployment.get_runner()
            runner.set_input({"images": golden_input}).run()
            saved_outputs_by_name = runner.get_outputs_dict()
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        saved_outputs, output_mapping = _match_outputs(saved_outputs_by_name)
        regression_logits, class_logits = assemble_six_head_logits(saved_outputs)
        reconstructed = reconstruct_yolov8_output(
            regression_logits,
            class_logits,
            QAT_FULL320_DFL_PROJECTION,
        )
    except Exception as exc:
        traceback.print_exc()
        print(f"saved-model gate failed before evaluation: {type(exc).__name__}: {exc}")
        return 2

    source_candidates, source_metrics = decode_yolo11_output(
        fp32_source_output,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    saved_candidates, saved_metrics = decode_yolo11_output(
        reconstructed,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        candidate_capacity=CANDIDATE_CAPACITY,
    )
    source_nms = global_nms(
        source_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=CANDIDATE_CAPACITY,
    )
    saved_nms = global_nms(
        saved_candidates,
        iou_threshold=IOU_THRESHOLD,
        result_capacity=CANDIDATE_CAPACITY,
    )

    evidence_dir = output_dir / "evidence"
    tensor_paths = {
        logical_name: evidence_dir / f"{logical_name}_fp32.npy"
        for logical_name in SIX_HEAD_OUTPUT_NAMES
    }
    tensor_paths.update(
        {
            "input": evidence_dir / "input_fp32.npy",
            "regression_logits": evidence_dir / "regression_logits_fp32.npy",
            "class_logits": evidence_dir / "class_logits_fp32.npy",
            "reconstructed_output": evidence_dir / "reconstructed_output_fp32.npy",
        }
    )
    tensor_values = {
        **dict(zip(SIX_HEAD_OUTPUT_NAMES, saved_outputs)),
        "input": golden_input,
        "regression_logits": regression_logits,
        "class_logits": class_logits,
        "reconstructed_output": reconstructed,
    }
    for name, path in tensor_paths.items():
        _write_npy(path, tensor_values[name])

    minimum_candidates = max(1, math.floor(len(source_candidates) * 0.25))
    minimum_nms = max(1, math.floor(len(source_nms) * 0.50))
    raw_output_checks = {
        logical_name: {
            "finite": bool(np.all(np.isfinite(values))),
            "noncollapsed": bool(float(np.ptp(values)) > 0.0),
        }
        for logical_name, values in zip(SIX_HEAD_OUTPUT_NAMES, saved_outputs)
    }
    acceptance = {
        "all_six_outputs_finite": all(
            item["finite"] for item in raw_output_checks.values()
        ),
        "all_six_outputs_noncollapsed": all(
            item["noncollapsed"] for item in raw_output_checks.values()
        ),
        "reconstructed_output_finite": bool(np.all(np.isfinite(reconstructed))),
        "candidate_count_at_least_25_percent_of_fp32": (
            len(saved_candidates) >= minimum_candidates
        ),
        "valid_geometry_present": saved_metrics["valid_geometry"] > 0,
        "nonfinite_geometry_absent": saved_metrics["nonfinite_rejected"] == 0,
        "nms_count_at_least_50_percent_of_fp32": len(saved_nms) >= minimum_nms,
        "nms_fits_firmware_capacity": len(saved_nms) <= FIRMWARE_RESULT_CAPACITY,
    }
    qualified = all(acceptance.values())
    source_candidate_indices = [item.candidate_index for item in source_candidates]
    saved_candidate_indices = [item.candidate_index for item in saved_candidates]
    source_nms_indices = [item.candidate_index for item in source_nms]
    saved_nms_indices = [item.candidate_index for item in saved_nms]
    report = {
        "schema_version": 1,
        "status": "saved_model_qualified" if qualified else "saved_model_rejected",
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "mera": mera.__version__,
            "loader": "ModelLoader.from_quantized_mera",
            "deployment_platform": "Platform.MCU_CPU",
            "deployment_target": "Target.MERAInterpreter",
        },
        "artifacts": {
            "saved_model": {
                "path": model_path.as_posix(),
                "bytes": model_path.stat().st_size,
                "sha256": model_sha256,
            },
            "golden_input": {
                "path": golden_path.as_posix(),
                "sha256": _sha256_file(golden_path),
                "tensor_sha256": _tensor_sha256(golden_input),
            },
            "fp32_source_output": {
                "path": source_output_path.as_posix(),
                "sha256": _sha256_file(source_output_path),
                "tensor_sha256": _tensor_sha256(fp32_source_output),
            },
        },
        "elapsed_ms": elapsed_ms,
        "output_mapping": output_mapping,
        "outputs": {
            name: _output_record(tensor_paths[name], tensor_values[name])
            for name in tensor_paths
            if name != "input"
        },
        "raw_output_checks": raw_output_checks,
        "thresholds": {
            "confidence": CONFIDENCE_THRESHOLD,
            "iou": IOU_THRESHOLD,
            "candidate_capacity": CANDIDATE_CAPACITY,
            "firmware_result_capacity": FIRMWARE_RESULT_CAPACITY,
            "minimum_saved_candidate_count": minimum_candidates,
            "minimum_saved_nms_count": minimum_nms,
        },
        "fp32_reference": {
            "candidate_metrics": source_metrics,
            "candidate_count": len(source_candidates),
            "candidate_indices": source_candidate_indices,
            "nms_count": len(source_nms),
            "nms_indices": source_nms_indices,
            "score_max": float(np.max(fp32_source_output[:, 4, :])),
        },
        "saved_model_result": {
            "candidate_metrics": saved_metrics,
            "candidate_count": len(saved_candidates),
            "candidate_indices": saved_candidate_indices,
            "nms_count": len(saved_nms),
            "nms_indices": saved_nms_indices,
            "score_max": float(np.max(reconstructed[:, 4, :])),
            "candidate_index_intersection_count": len(
                set(source_candidate_indices) & set(saved_candidate_indices)
            ),
            "nms_index_intersection_count": len(
                set(source_nms_indices) & set(saved_nms_indices)
            ),
        },
        "acceptance": acceptance,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    report_path = output_dir / "saved_model_gate.json"
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "saved-model-gate "
        f"status={report['status']} "
        f"candidates={len(saved_candidates)}/{len(source_candidates)} "
        f"nms={len(saved_nms)}/{len(source_nms)} "
        f"score_max={report['saved_model_result']['score_max']:.9f} "
        f"report={report_path}"
    )
    return 0 if qualified else 3


if __name__ == "__main__":
    raise SystemExit(main())
