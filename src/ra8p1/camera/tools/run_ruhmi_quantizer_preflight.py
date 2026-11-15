#!/usr/bin/env python3
"""Run the exact MERA Quantizer-constructor frontend gate once."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
import traceback


SYMBOLIC_DIMS = {
    "batch": 1,
    "batch_size": 1,
    "width": 224,
    "height": 224,
    "num_channels": 3,
    "N": 1,
}


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _artifact_records(root: Path) -> list[dict[str, object]]:
    return [
        {
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": _sha256_file(path),
        }
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="One-shot pinned MERA Quantizer constructor preflight"
    )
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--expected-model-sha256", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    model_path = args.model.resolve()
    output_dir = args.output_dir.resolve()
    if not model_path.is_file():
        parser.error(f"model does not exist: {model_path}")
    actual_sha256 = _sha256_file(model_path)
    if actual_sha256 != args.expected_model_sha256.lower():
        parser.error(
            "model hash mismatch: "
            f"expected={args.expected_model_sha256.lower()} actual={actual_sha256}"
        )
    if output_dir.exists():
        parser.error(
            "output directory already exists; refusing a preflight retry: "
            f"{output_dir}"
        )

    try:
        import mera
        from mera import Platform, quantizer
    except ImportError as exc:
        parser.error(f"pinned MERA environment is not active: {exc}")

    identity = {
        "event": "ruhmi_quantizer_constructor_preflight_start",
        "python": sys.version,
        "python_executable": sys.executable,
        "platform": platform.platform(),
        "mera": mera.__version__,
        "model": str(model_path),
        "model_bytes": model_path.stat().st_size,
        "model_sha256": actual_sha256,
        "output_dir": str(output_dir),
        "shape_mapping": SYMBOLIC_DIMS,
        "quantizer_config": "QuantizerConfigPresets.MCU",
        "mera_platform": "Platform.MCU_ETHOS",
        "forbidden_calls": [
            "Quantizer.calibrate",
            "Quantizer.quantize",
            "Vela",
            "MCU code generation",
        ],
    }
    print(json.dumps(identity, indent=2, sort_keys=True), flush=True)

    try:
        with mera.Deployer(str(output_dir), overwrite=False) as deployer:
            model = mera.ModelLoader(deployer).from_onnx(
                str(model_path),
                shape_mapping=SYMBOLIC_DIMS,
            )
            print(
                json.dumps(
                    {
                        "event": "model_loader_input_description",
                        "inputs": {
                            name: {
                                "shape": list(item.input_shape),
                                "dtype": str(item.input_type),
                            }
                            for name, item in model.input_desc.all_inputs.items()
                        },
                    },
                    indent=2,
                    sort_keys=True,
                ),
                flush=True,
            )
            qtzer = mera.Quantizer(
                deployer,
                model,
                quantizer_config=quantizer.QuantizerConfigPresets.MCU,
                mera_platform=Platform.MCU_ETHOS,
            )
            print(
                json.dumps(
                    {
                        "event": "ruhmi_quantizer_constructor_preflight_pass",
                        "input_ids": sorted(qtzer.in_info),
                        "output_ids": sorted(qtzer.out_info),
                    },
                    indent=2,
                    sort_keys=True,
                ),
                flush=True,
            )
    except Exception as exc:
        traceback.print_exc()
        print(
            json.dumps(
                {
                    "event": "ruhmi_quantizer_constructor_preflight_fail",
                    "exception_type": type(exc).__name__,
                    "exception": str(exc),
                    "artifacts": (
                        _artifact_records(output_dir) if output_dir.exists() else []
                    ),
                },
                indent=2,
                sort_keys=True,
            ),
            flush=True,
        )
        return 1

    print(
        json.dumps(
            {
                "event": "ruhmi_quantizer_constructor_preflight_complete",
                "status": "passed",
                "calibration_calls": 0,
                "quantization_calls": 0,
                "vela_calls": 0,
                "codegen_calls": 0,
                "artifacts": _artifact_records(output_dir),
            },
            indent=2,
            sort_keys=True,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
