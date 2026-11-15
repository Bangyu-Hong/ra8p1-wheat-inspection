#!/usr/bin/env python3
"""Run the production YOLOX TFLite contract on one raw VGA camera frame."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np

from wheat_deployment.preprocess import rgb565_be_to_rgb888
from wheat_deployment.yolox_postprocess import (
    YoloxDetection,
    decode_yolox_raw,
    yolox_class_agnostic_nms,
)


TILE_ORIGINS = tuple(
    (x, y) for y in (0, 179, 256) for x in (0, 179, 358, 416)
)
MODEL_SIZE = 224
FRAME_WIDTH = 640
FRAME_HEIGHT = 480
CONFIDENCE = 0.25
NMS_IOU = 0.45
FIRMWARE_CANDIDATE_CAPACITY = 300
SDRAM_SWEEP_SEED = np.uint32(0xA5A5A5A5)
SDRAM_SWEEP_MULTIPLIER = np.uint32(0x9E3779B9)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def _sdram_sweep_match_fraction(frame: bytes) -> float:
    if len(frame) % 4:
        return 0.0
    words = np.frombuffer(frame, dtype="<u4")
    indices = np.arange(words.size, dtype=np.uint32)
    expected = SDRAM_SWEEP_SEED ^ (indices * SDRAM_SWEEP_MULTIPLIER)
    return float(np.mean(words == expected))


def _firmware_keep_candidate(
    candidates: list[YoloxDetection], candidate: YoloxDetection
) -> None:
    if len(candidates) < FIRMWARE_CANDIDATE_CAPACITY:
        candidates.append(candidate)
        return

    minimum_index = min(
        range(len(candidates)), key=lambda index: candidates[index].confidence
    )
    if candidate.confidence > candidates[minimum_index].confidence:
        candidates[minimum_index] = candidate


def _restore_to_frame(
    detection: YoloxDetection, tile_index: int, tile_x: int, tile_y: int
) -> YoloxDetection:
    return YoloxDetection(
        candidate_index=tile_index * 1029 + detection.candidate_index,
        confidence=detection.confidence,
        x1=detection.x1 + tile_x,
        y1=detection.y1 + tile_y,
        x2=detection.x2 + tile_x,
        y2=detection.y2 + tile_y,
    )


def _clip_to_frame(detection: YoloxDetection) -> YoloxDetection | None:
    x1 = max(0.0, detection.x1)
    y1 = max(0.0, detection.y1)
    x2 = min(float(FRAME_WIDTH), detection.x2)
    y2 = min(float(FRAME_HEIGHT), detection.y2)
    if x2 <= x1 or y2 <= y1:
        return None
    return YoloxDetection(
        candidate_index=detection.candidate_index,
        confidence=detection.confidence,
        x1=x1,
        y1=y1,
        x2=x2,
        y2=y2,
    )


def _serialize(detection: YoloxDetection) -> dict[str, int | float]:
    return {
        "candidate_index": detection.candidate_index,
        "confidence": detection.confidence,
        "x1": detection.x1,
        "y1": detection.y1,
        "x2": detection.x2,
        "y2": detection.y2,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frame", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    frame_bytes = args.frame.read_bytes()
    rgb = rgb565_be_to_rgb888(frame_bytes)
    bgr = np.ascontiguousarray(rgb[:, :, ::-1])
    sweep_match_fraction = _sdram_sweep_match_fraction(frame_bytes)
    if sweep_match_fraction == 1.0:
        args.output.mkdir(parents=True, exist_ok=True)
        invalid_path = args.output / "invalid_sdram_sweep_rgb888.png"
        report_path = args.output / "live_frame_tflite_int8_report.json"
        cv2.imwrite(str(invalid_path), bgr)
        report = {
            "status": "rejected_sdram_deterministic_sweep",
            "frame": str(args.frame.resolve()),
            "frame_sha256": _sha256(args.frame),
            "sdram_sweep_match_fraction": sweep_match_fraction,
            "reason": (
                "The dump was captured after reset and contains the full SDRAM "
                "startup-test pattern, not a camera frame."
            ),
            "inference_executed": False,
            "invalid_frame_png": str(invalid_path.resolve()),
        }
        report_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        print(json.dumps(report, indent=2, ensure_ascii=False))
        raise RuntimeError(report["reason"])

    import tensorflow as tf

    interpreter = tf.lite.Interpreter(model_path=str(args.model), num_threads=4)
    interpreter.allocate_tensors()
    input_info = interpreter.get_input_details()[0]
    output_info = interpreter.get_output_details()[0]
    if tuple(input_info["shape"]) != (1, MODEL_SIZE, MODEL_SIZE, 3):
        raise ValueError(f"unexpected input contract: {input_info}")
    if input_info["dtype"] != np.int8 or input_info["quantization"] != (1.0, -128):
        raise ValueError(f"unexpected input quantization: {input_info}")
    if tuple(output_info["shape"]) != (1, 1029, 6):
        raise ValueError(f"unexpected output contract: {output_info}")
    if output_info["dtype"] != np.int8:
        raise ValueError(f"unexpected output dtype: {output_info}")

    candidates: list[YoloxDetection] = []
    candidates_above_threshold = 0
    per_tile: list[dict[str, int]] = []
    output_scale, output_zero_point = output_info["quantization"]
    for tile_index, (tile_x, tile_y) in enumerate(TILE_ORIGINS):
        tile_bgr = bgr[tile_y : tile_y + MODEL_SIZE,
                       tile_x : tile_x + MODEL_SIZE]
        model_input = (tile_bgr.astype(np.int16) - 128).astype(np.int8)[None]
        interpreter.set_tensor(input_info["index"], model_input)
        interpreter.invoke()
        quantized_output = interpreter.get_tensor(output_info["index"])
        raw_output = (
            quantized_output.astype(np.float32) - float(output_zero_point)
        ) * float(output_scale)
        local = decode_yolox_raw(
            raw_output.astype(np.float32, copy=False),
            confidence_threshold=CONFIDENCE,
        )
        kept = 0
        for detection in local:
            candidates_above_threshold += 1
            restored = _restore_to_frame(
                detection, tile_index, tile_x, tile_y
            )
            _firmware_keep_candidate(candidates, restored)
            if _clip_to_frame(restored) is not None:
                kept += 1
        per_tile.append(
            {
                "tile_index": tile_index,
                "tile_x": tile_x,
                "tile_y": tile_y,
                "above_threshold": len(local),
                "valid_geometry": kept,
            }
        )

    mapped = [
        clipped
        for candidate in candidates
        if (clipped := _clip_to_frame(candidate)) is not None
    ]
    retained = yolox_class_agnostic_nms(mapped, iou_threshold=NMS_IOU)
    args.output.mkdir(parents=True, exist_ok=True)
    original_path = args.output / "frame_rgb888.png"
    overlay_path = args.output / "frame_tflite_int8_detections.png"
    report_path = args.output / "live_frame_tflite_int8_report.json"
    cv2.imwrite(str(original_path), bgr)
    overlay = bgr.copy()
    for index, detection in enumerate(retained, start=1):
        point1 = (int(round(detection.x1)), int(round(detection.y1)))
        point2 = (int(round(detection.x2)), int(round(detection.y2)))
        cv2.rectangle(overlay, point1, point2, (0, 255, 0), 2)
        cv2.putText(
            overlay,
            f"{index}:{detection.confidence:.2f}",
            (point1[0], max(12, point1[1] - 3)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.38,
            (0, 255, 0),
            1,
            cv2.LINE_AA,
        )
    cv2.putText(
        overlay,
        f"count={len(retained)} conf={CONFIDENCE:.2f} nms={NMS_IOU:.2f}",
        (8, 22),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (0, 0, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.imwrite(str(overlay_path), overlay)

    report = {
        "frame": str(args.frame.resolve()),
        "frame_sha256": _sha256(args.frame),
        "sdram_sweep_match_fraction": sweep_match_fraction,
        "model": str(args.model.resolve()),
        "model_sha256": _sha256(args.model),
        "input_contract": {
            "frame": "640x480 RGB565-BE",
            "tiles": 12,
            "tile_size": 224,
            "tile_origins": TILE_ORIGINS,
            "tensor": "NHWC INT8 BGR",
            "scale": 1.0,
            "zero_point": -128,
        },
        "thresholds": {"confidence": CONFIDENCE, "global_nms_iou": NMS_IOU},
        "candidates_above_threshold": candidates_above_threshold,
        "candidate_pool_count": len(candidates),
        "candidate_pool_dropped": max(
            0, candidates_above_threshold - FIRMWARE_CANDIDATE_CAPACITY
        ),
        "retained_count": len(retained),
        "per_tile": per_tile,
        "detections": [_serialize(detection) for detection in retained],
        "outputs": {
            "original_png": str(original_path.resolve()),
            "overlay_png": str(overlay_path.resolve()),
        },
    }
    report_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
