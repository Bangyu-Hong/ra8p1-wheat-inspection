#!/usr/bin/env python3
"""Validate the frozen YOLOX tile224 source and generated pre-Vela outputs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import sys
import traceback

import numpy as np

from wheat_deployment.yolox_numeric_gate import run_numeric_gate


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-model", required=True, type=Path)
    parser.add_argument("--pre-vela-model", required=True, type=Path)
    parser.add_argument("--output-report", required=True, type=Path)
    args = parser.parse_args(argv)

    if args.output_report.exists():
        parser.error(f"output report already exists: {args.output_report}")

    try:
        import tensorflow as tf

        report = run_numeric_gate(
            args.source_model,
            args.pre_vela_model,
            interpreter_factory=tf.lite.Interpreter,
        )
        report["environment"] = {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "tensorflow": tf.__version__,
        }
    except Exception as exc:
        traceback.print_exc()
        report = {
            "schema_version": 1,
            "status": "failed",
            "scope": "source-tflite-and-generated-pre-vela-numeric-parity",
            "board_numeric_gate_proven": False,
            "failures": [f"{type(exc).__name__}: {exc}"],
        }

    args.output_report.parent.mkdir(parents=True, exist_ok=True)
    args.output_report.write_text(
        json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    sanity = report.get("cases", {}).get("all_minus_128", {})
    source_crc = sanity.get("source_output", {}).get("crc32", "unavailable")
    pre_vela_crc = sanity.get("pre_vela_output", {}).get("crc32", "unavailable")
    print(
        f"yolox-tile224-numeric status={report['status']} "
        f"source_crc={source_crc} pre_vela_crc={pre_vela_crc} "
        f"report={args.output_report}"
    )
    return 0 if report["status"] == "passed" else 3


if __name__ == "__main__":
    raise SystemExit(main())
