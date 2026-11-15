#!/usr/bin/env python3
"""Reload and evaluate a saved MERA YOLOX full224 diagnostic model."""

from __future__ import annotations

import argparse
import csv
import hashlib
from io import BytesIO, StringIO
import json
from pathlib import Path
import platform
import sys
import time
import traceback
from zipfile import ZipFile

import numpy as np

from wheat_deployment.yolox_postprocess import (
    decode_yolox_raw,
    yolox_class_agnostic_nms,
)


CONFIDENCE_THRESHOLD = 0.25
IOU_THRESHOLD = 0.45


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--golden-zip", required=True, type=Path)
    parser.add_argument("--count-metrics", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    required = (args.model, args.golden_zip, args.count_metrics)
    for path in required:
        if not path.is_file():
            parser.error(f"required artifact does not exist: {path}")
    if args.output_dir.exists():
        parser.error(f"output directory already exists: {args.output_dir}")

    expected_counts: dict[str, int] = {}
    with args.count_metrics.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            expected_counts[row["file_name"]] = int(row["predicted_count"])

    try:
        import mera
        from mera import Platform, Target
    except ImportError as exc:
        parser.error(f"pinned MERA environment is not active: {exc}")

    args.output_dir.mkdir(parents=True)
    cases: list[dict[str, object]] = []
    try:
        with ZipFile(args.golden_zip) as archive:
            golden_manifest = list(
                csv.DictReader(
                    StringIO(archive.read("manifest.csv").decode("utf-8-sig"))
                )
            )
            with mera.Deployer(
                str(args.output_dir / "deployment"), overwrite=False
            ) as deployer:
                model = mera.ModelLoader(deployer).from_quantized_mera(
                    str(args.model.resolve())
                )
                deployment = deployer.deploy(
                    model,
                    mera_platform=Platform.MCU_CPU,
                    target=Target.MERAInterpreter,
                )
                runner = deployment.get_runner()
                for golden in golden_manifest:
                    file_name = golden["file_name"]
                    stem = Path(file_name).stem
                    model_input = np.load(
                        BytesIO(archive.read(f"inputs/{stem}.npy")),
                        allow_pickle=False,
                    )
                    fp32_raw = np.load(
                        BytesIO(archive.read(f"raw_outputs/onnx/{stem}.npy")),
                        allow_pickle=False,
                    )
                    started = time.perf_counter()
                    runner.set_input({"images": model_input}).run()
                    elapsed_ms = (time.perf_counter() - started) * 1000.0
                    outputs = runner.get_outputs_dict()
                    if len(outputs) != 1:
                        raise ValueError(
                            f"saved model exposed {len(outputs)} outputs for {file_name}"
                        )
                    output_name, saved_raw = next(iter(outputs.items()))
                    saved_raw = np.asarray(saved_raw, dtype=np.float32)
                    if saved_raw.shape != (1, 1029, 6):
                        raise ValueError(
                            f"saved output shape mismatch for {file_name}: {saved_raw.shape}"
                        )

                    fp32_candidates = decode_yolox_raw(
                        fp32_raw,
                        confidence_threshold=CONFIDENCE_THRESHOLD,
                    )
                    fp32_nms = yolox_class_agnostic_nms(
                        fp32_candidates,
                        iou_threshold=IOU_THRESHOLD,
                    )
                    expected_count = expected_counts[file_name]
                    if len(fp32_nms) != expected_count:
                        raise ValueError(
                            f"FP32 decoder count mismatch for {file_name}: "
                            f"expected {expected_count}, observed {len(fp32_nms)}"
                        )
                    saved_candidates = decode_yolox_raw(
                        saved_raw,
                        confidence_threshold=CONFIDENCE_THRESHOLD,
                    )
                    saved_nms = yolox_class_agnostic_nms(
                        saved_candidates,
                        iou_threshold=IOU_THRESHOLD,
                    )
                    fp32_candidate_indices = {
                        item.candidate_index for item in fp32_candidates
                    }
                    saved_candidate_indices = {
                        item.candidate_index for item in saved_candidates
                    }
                    fp32_nms_indices = {item.candidate_index for item in fp32_nms}
                    saved_nms_indices = {item.candidate_index for item in saved_nms}
                    cases.append(
                        {
                            "file_name": file_name,
                            "output_name": output_name,
                            "elapsed_ms": elapsed_ms,
                            "finite": bool(np.all(np.isfinite(saved_raw))),
                            "nonconstant": bool(float(np.ptp(saved_raw)) > 0.0),
                            "fp32": {
                                "candidate_count": len(fp32_candidates),
                                "nms_count": len(fp32_nms),
                                "confidence_max": float(
                                    np.max(fp32_raw[..., 4] * fp32_raw[..., 5])
                                ),
                            },
                            "saved": {
                                "candidate_count": len(saved_candidates),
                                "nms_count": len(saved_nms),
                                "confidence_max": float(
                                    np.max(saved_raw[..., 4] * saved_raw[..., 5])
                                ),
                                "candidate_index_intersection": len(
                                    fp32_candidate_indices & saved_candidate_indices
                                ),
                                "nms_index_intersection": len(
                                    fp32_nms_indices & saved_nms_indices
                                ),
                            },
                        }
                    )
    except Exception as exc:
        traceback.print_exc()
        print(f"YOLOX saved-model gate failed: {type(exc).__name__}: {exc}")
        return 2

    fp32_candidates_total = sum(case["fp32"]["candidate_count"] for case in cases)
    saved_candidates_total = sum(case["saved"]["candidate_count"] for case in cases)
    fp32_nms_total = sum(case["fp32"]["nms_count"] for case in cases)
    saved_nms_total = sum(case["saved"]["nms_count"] for case in cases)
    candidate_overlap_total = sum(
        case["saved"]["candidate_index_intersection"] for case in cases
    )
    nms_overlap_total = sum(case["saved"]["nms_index_intersection"] for case in cases)
    count_absolute_errors = [
        abs(case["saved"]["nms_count"] - case["fp32"]["nms_count"])
        for case in cases
    ]
    summary = {
        "case_count": len(cases),
        "all_finite": all(case["finite"] for case in cases),
        "all_nonconstant": all(case["nonconstant"] for case in cases),
        "fp32_decoder_matches_delivered_counts": True,
        "fp32_candidate_total": fp32_candidates_total,
        "saved_candidate_total": saved_candidates_total,
        "candidate_retention": saved_candidates_total / max(fp32_candidates_total, 1),
        "candidate_index_recall": candidate_overlap_total
        / max(fp32_candidates_total, 1),
        "fp32_nms_total": fp32_nms_total,
        "saved_nms_total": saved_nms_total,
        "nms_count_retention": saved_nms_total / max(fp32_nms_total, 1),
        "nms_index_recall": nms_overlap_total / max(fp32_nms_total, 1),
        "count_mae_against_fp32": float(np.mean(count_absolute_errors)),
        "count_max_absolute_error": max(count_absolute_errors, default=0),
        "mean_interpreter_run_ms": float(
            np.mean([case["elapsed_ms"] for case in cases])
        ),
    }
    acceptance = {
        "all_outputs_finite": summary["all_finite"],
        "all_outputs_nonconstant": summary["all_nonconstant"],
        "candidate_retention_at_least_25_percent": (
            summary["candidate_retention"] >= 0.25
        ),
        "candidate_index_recall_at_least_25_percent": (
            summary["candidate_index_recall"] >= 0.25
        ),
        "nms_count_retention_at_least_50_percent": (
            summary["nms_count_retention"] >= 0.50
        ),
        "nms_index_recall_at_least_25_percent": (
            summary["nms_index_recall"] >= 0.25
        ),
    }
    qualified = all(acceptance.values())
    report = {
        "schema_version": 1,
        "status": "saved_model_qualified" if qualified else "saved_model_rejected",
        "purpose": "full224-chain-smoke",
        "final_tile_model": False,
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "mera": mera.__version__,
            "loader": "ModelLoader.from_quantized_mera",
            "target": "MERAInterpreter",
        },
        "artifacts": {
            "model": {
                "path": str(args.model.resolve()),
                "bytes": args.model.stat().st_size,
                "sha256": sha256_file(args.model),
            },
            "golden_zip": {
                "path": str(args.golden_zip.resolve()),
                "bytes": args.golden_zip.stat().st_size,
                "sha256": sha256_file(args.golden_zip),
            },
            "count_metrics": {
                "path": str(args.count_metrics.resolve()),
                "bytes": args.count_metrics.stat().st_size,
                "sha256": sha256_file(args.count_metrics),
            },
        },
        "thresholds": {
            "confidence": CONFIDENCE_THRESHOLD,
            "iou": IOU_THRESHOLD,
        },
        "summary": summary,
        "acceptance": acceptance,
        "cases": cases,
    }
    report_path = args.output_dir / "saved_model_gate.json"
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"yolox-saved-model-gate status={report['status']} "
        f"candidates={saved_candidates_total}/{fp32_candidates_total} "
        f"nms={saved_nms_total}/{fp32_nms_total} "
        f"nms_index_recall={summary['nms_index_recall']:.6f} "
        f"count_mae={summary['count_mae_against_fp32']:.3f} "
        f"report={report_path}"
    )
    return 0 if qualified else 3


if __name__ == "__main__":
    raise SystemExit(main())
