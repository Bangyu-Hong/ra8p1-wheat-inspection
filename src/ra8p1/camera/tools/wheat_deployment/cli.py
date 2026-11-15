"""Command-line entry point for reproducible host deployment evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import sys
import time

import numpy as np

from .contract import (
    CONTRACT_PATH,
    get_output_contracts,
    load_model_contract,
    sha256_file,
    verify_model_contract,
)
from .manifest import (
    build_gwhd_manifest,
    load_gwhd_vga_rgb,
    load_manifest,
    prepare_calibration_tensors,
    verify_gwhd_manifest,
    write_manifest,
)
from .head_transform import derive_six_head_output_model
from .model_transform import (
    derive_predecode_output_model,
    derive_split_output_model,
    wrap_predecode_output_identities,
)
from .postprocess import assemble_yolo_output, decode_yolo11_output, global_nms
from .preprocess import build_fp32_model_input
from .qualification import qualify_predecode_candidate, qualify_six_head_candidate
from .yolox_calibration import prepare_yolox_calibration


MANIFEST_PATH = Path(__file__).with_name("manifests") / "gwhd_calibration_golden.json"


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "build-manifest":
            manifest = build_gwhd_manifest(args.dataset_root)
            write_manifest(manifest, args.output)
            print(
                "manifest status=ok "
                f"calibration={manifest['selection']['calibration_image_count']} "
                f"golden={manifest['selection']['golden_image_count']} "
                f"sha256={manifest['manifest_sha256']}"
            )
        elif args.command == "verify-manifest":
            manifest = load_manifest(args.manifest)
            result = verify_gwhd_manifest(args.dataset_root, manifest)
            print(
                "manifest-verify status=ok "
                f"annotations={result['annotation_files']} images={result['images']} "
                f"sha256={manifest['manifest_sha256']}"
            )
        elif args.command == "verify-model":
            result = verify_model_contract(args.model, args.contract)
            print(
                "model-verify status=ok "
                f"bytes={result['bytes']} sha256={result['sha256']} "
                f"onnx_checked={str(result['onnx_checked']).lower()}"
            )
        elif args.command == "prepare-calibration":
            manifest = load_manifest(args.manifest)
            result = prepare_calibration_tensors(
                args.dataset_root,
                manifest,
                args.output_dir,
                model_size=args.model_size,
            )
            print(
                "calibration status=ok "
                f"tensors={result['tensor_count']} sha256={result['index_sha256']}"
            )
        elif args.command == "derive-split-output-model":
            result = derive_split_output_model(args.model, args.output)
            print(
                "model-derive status=ok "
                f"transform={result['transform_id']} "
                f"source_sha256={result['source_sha256']} "
                f"derived_bytes={result['derived_bytes']} "
                f"derived_sha256={result['derived_sha256']}"
            )
        elif args.command == "derive-predecode-output-model":
            result = derive_predecode_output_model(args.model, args.output)
            print(
                "model-derive status=ok "
                f"transform={result['transform_id']} "
                f"source_sha256={result['source_sha256']} "
                f"derived_bytes={result['derived_bytes']} "
                f"derived_sha256={result['derived_sha256']}"
            )
        elif args.command == "wrap-predecode-output-identities":
            result = wrap_predecode_output_identities(args.model, args.output)
            print(
                "model-wrap status=ok "
                f"transform={result['transform_id']} "
                f"source_sha256={result['source_sha256']} "
                f"wrapped_bytes={result['wrapped_bytes']} "
                f"wrapped_sha256={result['wrapped_sha256']}"
            )
        elif args.command == "qualify-predecode-candidate":
            result = qualify_predecode_candidate(
                args.model,
                args.golden_input,
                args.output_dir,
                expected_source_sha256=args.expected_source_sha256,
            )
            golden = result["cases"][0]
            print(
                "predecode-qualify status=ok "
                f"cases={len(result['cases'])} "
                f"golden_candidates={golden['retained_candidate_count']} "
                f"golden_nms={golden['nms_count']} "
                f"direct_sha256={result['artifacts']['direct']['sha256']} "
                f"wrapped_sha256={result['artifacts']['identity_wrapped']['sha256']}"
            )
        elif args.command == "derive-six-head-output-model":
            result = derive_six_head_output_model(args.model, args.output)
            print(
                "model-derive status=ok "
                f"transform={result['transform_id']} "
                f"source_sha256={result['source_sha256']} "
                f"derived_bytes={result['derived_bytes']} "
                f"derived_sha256={result['derived_sha256']}"
            )
        elif args.command == "qualify-six-head-candidate":
            result = qualify_six_head_candidate(
                args.model,
                args.golden_input,
                args.output_dir,
                expected_source_sha256=args.expected_source_sha256,
            )
            golden = result["cases"][0]
            print(
                "six-head-qualify status=ok "
                f"cases={len(result['cases'])} "
                f"golden_candidates={golden['retained_candidate_count']} "
                f"golden_nms={golden['nms_count']} "
                f"candidate_sha256={result['artifacts']['six_head']['sha256']}"
            )
        elif args.command == "run-reference":
            _run_reference(args)
        elif args.command == "prepare-yolox-calibration":
            result = prepare_yolox_calibration(
                args.dataset_root,
                args.split_manifest,
                args.golden_zip,
                args.output_dir,
                count=args.count,
            )
            print(
                "yolox-calibration status=ok "
                f"purpose={result['purpose']} tensors={result['tensor_count']} "
                f"golden={result['golden_case_count']} "
                f"golden_max_diff={result['golden_maximum_difference']} "
                f"manifest_sha256={result['manifest_sha256']}"
            )
        else:
            parser.error(f"unsupported command: {args.command}")
    except (FileNotFoundError, RuntimeError, TypeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="RA8P1 YOLO wheat host deployment reference"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_manifest = subparsers.add_parser("build-manifest")
    build_manifest.add_argument("--dataset-root", required=True, type=Path)
    build_manifest.add_argument("--output", type=Path, default=MANIFEST_PATH)

    verify_manifest = subparsers.add_parser("verify-manifest")
    verify_manifest.add_argument("--dataset-root", required=True, type=Path)
    verify_manifest.add_argument("--manifest", type=Path, default=MANIFEST_PATH)

    verify_model = subparsers.add_parser("verify-model")
    verify_model.add_argument("--model", required=True, type=Path)
    verify_model.add_argument("--contract", type=Path, default=CONTRACT_PATH)

    prepare = subparsers.add_parser("prepare-calibration")
    prepare.add_argument("--dataset-root", required=True, type=Path)
    prepare.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    prepare.add_argument("--output-dir", required=True, type=Path)
    prepare.add_argument("--model-size", choices=(320, 640), default=640, type=int)

    derive = subparsers.add_parser("derive-split-output-model")
    derive.add_argument("--model", required=True, type=Path)
    derive.add_argument("--output", required=True, type=Path)

    derive_predecode = subparsers.add_parser("derive-predecode-output-model")
    derive_predecode.add_argument("--model", required=True, type=Path)
    derive_predecode.add_argument("--output", required=True, type=Path)

    wrap_predecode = subparsers.add_parser("wrap-predecode-output-identities")
    wrap_predecode.add_argument("--model", required=True, type=Path)
    wrap_predecode.add_argument("--output", required=True, type=Path)

    qualify_predecode = subparsers.add_parser("qualify-predecode-candidate")
    qualify_predecode.add_argument("--model", required=True, type=Path)
    qualify_predecode.add_argument("--golden-input", required=True, type=Path)
    qualify_predecode.add_argument("--output-dir", required=True, type=Path)
    qualify_predecode.add_argument("--expected-source-sha256", required=True)

    derive_six_head = subparsers.add_parser("derive-six-head-output-model")
    derive_six_head.add_argument("--model", required=True, type=Path)
    derive_six_head.add_argument("--output", required=True, type=Path)

    qualify_six_head = subparsers.add_parser("qualify-six-head-candidate")
    qualify_six_head.add_argument("--model", required=True, type=Path)
    qualify_six_head.add_argument("--golden-input", required=True, type=Path)
    qualify_six_head.add_argument("--output-dir", required=True, type=Path)
    qualify_six_head.add_argument("--expected-source-sha256", required=True)

    reference = subparsers.add_parser("run-reference")
    reference.add_argument("--model", required=True, type=Path)
    reference.add_argument("--image", required=True, type=Path)
    reference.add_argument("--output-dir", required=True, type=Path)
    reference.add_argument("--contract", type=Path, default=CONTRACT_PATH)
    reference.add_argument("--confidence", type=float, default=0.25)
    reference.add_argument("--iou", type=float, default=0.45)
    reference.add_argument("--candidate-capacity", type=int, default=300)
    reference.add_argument("--result-capacity", type=int, default=64)

    yolox_calibration = subparsers.add_parser("prepare-yolox-calibration")
    yolox_calibration.add_argument("--dataset-root", required=True, type=Path)
    yolox_calibration.add_argument("--split-manifest", required=True, type=Path)
    yolox_calibration.add_argument("--golden-zip", required=True, type=Path)
    yolox_calibration.add_argument("--output-dir", required=True, type=Path)
    yolox_calibration.add_argument("--count", type=int, default=200)
    return parser


def _run_reference(args: argparse.Namespace) -> None:
    model_identity = verify_model_contract(args.model, args.contract)
    contract = load_model_contract(args.contract)
    output_names = [output["name"] for output in get_output_contracts(contract)]
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError("onnxruntime is required for run-reference") from exc

    vga_rgb = load_gwhd_vga_rgb(args.image)
    model_size = contract["input"]["shape"][2]
    input_tensor = build_fp32_model_input(vga_rgb, model_size=model_size)
    session = ort.InferenceSession(str(args.model), providers=["CPUExecutionProvider"])
    started = time.perf_counter()
    model_outputs = session.run(output_names, {contract["input"]["name"]: input_tensor})
    inference_ms = (time.perf_counter() - started) * 1000.0
    projection_values = contract.get("postprocessing", {}).get("dfl_projection")
    dfl_projection = (
        None
        if projection_values is None
        else np.asarray(projection_values, dtype=np.float32)
    )
    raw_output = assemble_yolo_output(model_outputs, dfl_projection)
    candidates, decoder_metrics = decode_yolo11_output(
        raw_output,
        confidence_threshold=args.confidence,
        candidate_capacity=args.candidate_capacity,
    )
    detections = global_nms(
        candidates,
        iou_threshold=args.iou,
        result_capacity=args.result_capacity,
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    input_path = args.output_dir / "input_fp32.npy"
    raw_output_path = args.output_dir / "raw_output_fp32.npy"
    detections_path = args.output_dir / "detections.json"
    np.save(input_path, input_tensor, allow_pickle=False)
    np.save(raw_output_path, raw_output, allow_pickle=False)
    model_output_records: list[dict[str, object]] = []
    for output_index, (output_name, output_tensor) in enumerate(
        zip(output_names, model_outputs)
    ):
        output_path = args.output_dir / f"model_output_{output_index}_fp32.npy"
        np.save(output_path, output_tensor, allow_pickle=False)
        model_output_records.append(
            {
                "name": output_name,
                "shape": list(output_tensor.shape),
                "dtype": str(output_tensor.dtype),
                "file": output_path.name,
                "file_sha256": sha256_file(output_path),
            }
        )
    with detections_path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(
            {
                "candidate_metrics": decoder_metrics,
                "retained_count": len(detections),
                "detections": [detection.to_dict() for detection in detections],
            },
            stream,
            indent=2,
            sort_keys=True,
        )
        stream.write("\n")

    run_manifest = {
        "schema_version": 1,
        "python": sys.version,
        "platform": platform.platform(),
        "numpy": np.__version__,
        "onnxruntime": ort.__version__,
        "model_sha256": model_identity["sha256"],
        "model_outputs": model_output_records,
        "logical_output_assembly": (
            "identity"
            if len(model_outputs) == 1
            else "np.concatenate([boxes, scores], axis=1)"
        ),
        "source_image": str(args.image.resolve()),
        "source_image_sha256": sha256_file(args.image),
        "input_file": input_path.name,
        "input_sha256": sha256_file(input_path),
        "raw_output_file": raw_output_path.name,
        "raw_output_sha256": sha256_file(raw_output_path),
        "detections_file": detections_path.name,
        "detections_sha256": sha256_file(detections_path),
        "confidence_threshold": args.confidence,
        "iou_threshold": args.iou,
        "candidate_capacity": args.candidate_capacity,
        "result_capacity": args.result_capacity,
        "candidate_metrics": decoder_metrics,
        "retained_count": len(detections),
        "inference_ms": inference_ms,
    }
    run_manifest_path = args.output_dir / "run_manifest.json"
    write_manifest(run_manifest, run_manifest_path)
    print(
        "reference status=ok "
        f"candidates={len(candidates)} retained={len(detections)} "
        f"inference_ms={inference_ms:.3f} output={args.output_dir}"
    )
