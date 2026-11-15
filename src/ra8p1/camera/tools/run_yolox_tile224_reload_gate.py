#!/usr/bin/env python3
"""Fresh-process MERAInterpreter reload gate for one frozen YOLOX tile."""

from __future__ import annotations

import argparse
from pathlib import Path
import platform
import sys
import time
import traceback

import numpy as np

from wheat_deployment.yolox_tile224_gate import (
    EXPECTED_INPUT_SHAPE,
    EXPECTED_MODEL_SHA256,
    EXPECTED_OUTPUT_QUANTIZATION,
    EXPECTED_OUTPUT_SHAPE,
    compare_int8_raw_output,
    sha256_file,
    write_report,
)
from wheat_deployment.yolox_postprocess import (
    decode_yolox_raw,
    yolox_class_agnostic_nms,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--expected-output", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    for path in (args.model, args.input, args.expected_output):
        if not path.is_file():
            parser.error(f"required artifact does not exist: {path}")
    if args.output_dir.exists():
        parser.error(f"output directory already exists: {args.output_dir}")
    if sha256_file(args.model) != EXPECTED_MODEL_SHA256:
        parser.error("model hash does not match the frozen tile224 contract")

    supplied_input = np.load(args.input, allow_pickle=False)
    model_input, input_conversion = _prepare_model_input(supplied_input)
    supplied_expected = np.load(args.expected_output, allow_pickle=False)
    expected_output, expected_conversion = _prepare_expected_output(
        supplied_expected
    )

    try:
        import mera
        from mera import Platform, Target
    except ImportError as exc:
        parser.error(f"pinned MERA environment is not active: {exc}")

    args.output_dir.mkdir(parents=True)
    started = time.perf_counter()
    try:
        with mera.Deployer(
            str(args.output_dir / "deployment"), overwrite=False
        ) as deployer:
            model = mera.ModelLoader(deployer).from_tflite(
                str(args.model.resolve()), use_prequantize_input=True
            )
            input_names = sorted(model.input_desc.all_inputs)
            if len(input_names) != 1:
                raise ValueError(f"expected one MERA input, found {input_names}")
            deployment = deployer.deploy(
                model,
                mera_platform=Platform.MCU_CPU,
                target=Target.MERAInterpreter,
            )
            runner = deployment.get_runner()
            runner.set_input({input_names[0]: model_input}).run()
            outputs = runner.get_outputs_dict()
        if len(outputs) != 1:
            raise ValueError(f"expected one MERA output, found {list(outputs)}")
        output_name, actual_output = next(iter(outputs.items()))
        actual_output = np.asarray(actual_output)
        comparison = compare_int8_raw_output(actual_output, expected_output)
        postprocess = _compare_postprocess(actual_output, expected_output)
    except Exception as exc:
        traceback.print_exc()
        failure = {
            "schema_version": 1,
            "status": "source_tflite_reload_rejected",
            "firmware_integration_allowed": False,
            "compiled_model_reload_proven": False,
            "failures": [f"{type(exc).__name__}: {exc}"],
        }
        write_report(failure, args.output_dir / "tile224_reload_gate.json")
        return 2

    elapsed_ms = (time.perf_counter() - started) * 1000.0
    actual_path = args.output_dir / "actual_raw_output.npy"
    np.save(actual_path, actual_output, allow_pickle=False)
    qualified = comparison["status"] == "passed"
    report = {
        "schema_version": 1,
        "status": (
            "source_tflite_reload_qualified"
            if qualified
            else "source_tflite_reload_rejected"
        ),
        "scope": "fresh-process-source-tflite-mera-interpreter-single-tile",
        "firmware_integration_allowed": False,
        "compiled_model_reload_proven": False,
        "limitation": (
            "The quantize=false TFLite compile emitted C/Vela artifacts but no "
            "model.mera; this gate does not prove generated MCU numerical parity."
        ),
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "mera": mera.__version__,
            "loader": "ModelLoader.from_tflite",
            "use_prequantize_input": True,
            "target": "MERAInterpreter",
        },
        "artifacts": {
            "model": {
                "path": str(args.model.resolve()),
                "bytes": args.model.stat().st_size,
                "sha256": sha256_file(args.model),
            },
            "input": {
                "path": str(args.input.resolve()),
                "sha256": sha256_file(args.input),
            },
            "expected_output": {
                "path": str(args.expected_output.resolve()),
                "sha256": sha256_file(args.expected_output),
            },
            "actual_output": {
                "path": str(actual_path.resolve()),
                "sha256": sha256_file(actual_path),
            },
        },
        "input_name": input_names[0],
        "output_name": output_name,
        "input_conversion": input_conversion,
        "expected_output_conversion": expected_conversion,
        "elapsed_ms": elapsed_ms,
        "comparison": comparison,
        "postprocess": postprocess,
        "failures": comparison["failures"],
    }
    report_path = args.output_dir / "tile224_reload_gate.json"
    write_report(report, report_path)
    print(
        f"tile224-reload status={report['status']} "
        f"max_abs={comparison['maximum_absolute_error']} "
        f"changed={comparison['changed_values']} report={report_path}"
    )
    return 0 if qualified else 3


def _prepare_model_input(values: np.ndarray) -> tuple[np.ndarray, str]:
    if values.shape == tuple(EXPECTED_INPUT_SHAPE) and values.dtype == np.int8:
        return values, "already-int8-nhwc"
    if values.shape == (1, 3, 224, 224) and values.dtype == np.float32:
        nhwc = values.transpose(0, 2, 3, 1)
        quantized = np.clip(np.rint(nhwc) - 128, -128, 127).astype(np.int8)
        return quantized, "frozen-fp32-nchw-to-int8-nhwc-q1-zp-128"
    raise ValueError(
        "input must be int8 NHWC [1,224,224,3] or frozen float32 NCHW "
        f"[1,3,224,224], got {values.dtype} {list(values.shape)}"
    )


def _prepare_expected_output(values: np.ndarray) -> tuple[np.ndarray, str]:
    if values.shape != tuple(EXPECTED_OUTPUT_SHAPE):
        raise ValueError(
            f"expected output must have shape {EXPECTED_OUTPUT_SHAPE}, "
            f"got {list(values.shape)}"
        )
    if values.dtype == np.int8:
        return values, "already-int8"
    if values.dtype == np.float32:
        scale, zero_point = EXPECTED_OUTPUT_QUANTIZATION
        quantized = np.clip(
            np.rint(values / np.float32(scale) + zero_point), -128, 127
        ).astype(np.int8)
        return quantized, "dequantized-fp32-to-int8-q0.03774220868945122-zp-48"
    raise ValueError(f"expected output must be int8 or float32, got {values.dtype}")


def _compare_postprocess(
    actual: np.ndarray, expected: np.ndarray
) -> dict[str, object]:
    scale, zero_point = EXPECTED_OUTPUT_QUANTIZATION
    actual_fp32 = (actual.astype(np.float32) - zero_point) * np.float32(scale)
    expected_fp32 = (expected.astype(np.float32) - zero_point) * np.float32(scale)
    actual_candidates = decode_yolox_raw(actual_fp32)
    expected_candidates = decode_yolox_raw(expected_fp32)
    actual_nms = yolox_class_agnostic_nms(actual_candidates)
    expected_nms = yolox_class_agnostic_nms(expected_candidates)
    actual_candidate_indices = {item.candidate_index for item in actual_candidates}
    expected_candidate_indices = {item.candidate_index for item in expected_candidates}
    actual_nms_indices = {item.candidate_index for item in actual_nms}
    expected_nms_indices = {item.candidate_index for item in expected_nms}
    return {
        "confidence": 0.25,
        "nms_iou": 0.45,
        "actual_candidate_count": len(actual_candidates),
        "expected_candidate_count": len(expected_candidates),
        "candidate_index_intersection": len(
            actual_candidate_indices & expected_candidate_indices
        ),
        "actual_nms_count": len(actual_nms),
        "expected_nms_count": len(expected_nms),
        "nms_index_intersection": len(actual_nms_indices & expected_nms_indices),
    }


if __name__ == "__main__":
    raise SystemExit(main())
