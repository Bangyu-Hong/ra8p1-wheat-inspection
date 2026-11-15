"""Bounded YOLO11 wheat decoder and deterministic global NMS reference."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import heapq
import math
from typing import Iterable, Sequence

import numpy as np


MODEL_SIZE = 640.0
FRAME_WIDTH = 640.0
FRAME_HEIGHT = 480.0
LETTERBOX_TOP = 80.0
EXPECTED_CANDIDATES = 8400
YOLOV8_REG_MAX = 16


def build_yolov8_anchor_grid(
    model_size: int = 640,
) -> tuple[np.ndarray, np.ndarray]:
    """Return a fixed square YOLOv8 anchor grid for strides 8, 16, and 32."""

    if model_size not in (320, 640):
        raise ValueError("YOLOv8 anchor grid supports only model sizes 320 and 640")

    anchor_parts: list[np.ndarray] = []
    stride_parts: list[np.ndarray] = []
    for stride in (8, 16, 32):
        grid_size = model_size // stride
        coordinates = np.arange(grid_size, dtype=np.float32) + np.float32(0.5)
        grid_y, grid_x = np.meshgrid(coordinates, coordinates, indexing="ij")
        anchor_parts.append(np.stack((grid_x, grid_y), axis=0).reshape(2, -1))
        stride_parts.append(
            np.full(grid_size * grid_size, stride, dtype=np.float32)
        )
    anchor_points = np.concatenate(anchor_parts, axis=1)[None, :, :]
    strides = np.concatenate(stride_parts)[None, :]
    return anchor_points, strides


def reconstruct_yolov8_output(
    regression_logits: np.ndarray,
    class_logits: np.ndarray,
    dfl_projection: np.ndarray | None = None,
) -> np.ndarray:
    """Reconstruct exported xywh+confidence from pre-DFL YOLOv8 head logits."""

    if regression_logits.ndim != 3 or regression_logits.shape[:2] != (
        1,
        4 * YOLOV8_REG_MAX,
    ):
        raise ValueError(
            "regression_logits shape must be (1,64,2100) or (1,64,8400), "
            f"got {regression_logits.shape}"
        )
    candidate_count = regression_logits.shape[2]
    model_size_by_candidates = {2100: 320, EXPECTED_CANDIDATES: 640}
    if candidate_count not in model_size_by_candidates:
        raise ValueError(
            "regression_logits candidate count must be 2100 or 8400, "
            f"got {candidate_count}"
        )
    expected_regression_shape = (1, 4 * YOLOV8_REG_MAX, candidate_count)
    expected_class_shape = (1, 1, candidate_count)
    if class_logits.shape != expected_class_shape:
        raise ValueError(
            f"class_logits shape must be {expected_class_shape}, "
            f"got {class_logits.shape}"
        )
    if regression_logits.dtype != np.float32 or class_logits.dtype != np.float32:
        raise TypeError("YOLOv8 reconstruction inputs must both be float32")

    distributions = regression_logits.reshape(
        1,
        4,
        YOLOV8_REG_MAX,
        candidate_count,
    )
    shifted = distributions - np.max(distributions, axis=2, keepdims=True)
    exponentials = np.exp(shifted)
    probabilities = exponentials / np.sum(
        exponentials,
        axis=2,
        keepdims=True,
        dtype=np.float32,
    )
    if dfl_projection is None:
        projection = np.arange(YOLOV8_REG_MAX, dtype=np.float32)
    else:
        projection = np.asarray(dfl_projection)
        if projection.dtype != np.float32:
            raise TypeError("DFL projection must be float32")
        if projection.shape not in ((YOLOV8_REG_MAX,), (1, YOLOV8_REG_MAX, 1, 1)):
            raise ValueError(
                "DFL projection shape must be (16,) or (1,16,1,1), "
                f"got {projection.shape}"
            )
        if not np.all(np.isfinite(projection)):
            raise ValueError("DFL projection must contain only finite values")
        projection = projection.reshape(YOLOV8_REG_MAX)
    bins = projection.reshape(1, 1, -1, 1)
    distances = np.sum(
        probabilities * bins,
        axis=2,
        dtype=np.float32,
    )

    anchor_points, strides = build_yolov8_anchor_grid(
        model_size_by_candidates[candidate_count]
    )
    top_left = anchor_points - distances[:, :2, :]
    bottom_right = anchor_points + distances[:, 2:, :]
    centers = (top_left + bottom_right) / np.float32(2.0)
    sizes = bottom_right - top_left
    boxes = np.concatenate((centers, sizes), axis=1) * strides
    scores = _sigmoid_float32(class_logits)
    return np.concatenate((boxes, scores), axis=1)


def assemble_yolo_output(
    outputs: Sequence[np.ndarray],
    dfl_projection: np.ndarray | None = None,
) -> np.ndarray:
    """Reassemble the logical xywh+confidence tensor from supported model ABIs."""

    if len(outputs) == 1:
        return outputs[0]
    if len(outputs) == 6:
        regression_logits, class_logits = assemble_six_head_logits(outputs)
        return reconstruct_yolov8_output(
            regression_logits,
            class_logits,
            dfl_projection,
        )
    if len(outputs) != 2:
        raise ValueError(
            f"expected one, two, or six model outputs, got {len(outputs)}"
        )
    first, second = outputs
    if first.ndim == 3 and first.shape[:2] == (1, 64):
        return reconstruct_yolov8_output(first, second, dfl_projection)
    boxes, scores = first, second
    if boxes.ndim != 3 or boxes.shape[:2] != (1, 4):
        raise ValueError(f"boxes shape must be (1,4,N), got {boxes.shape}")
    candidate_count = boxes.shape[2]
    if candidate_count not in (2100, EXPECTED_CANDIDATES):
        raise ValueError(f"boxes candidate count must be 2100 or 8400, got {candidate_count}")
    if scores.shape != (1, 1, candidate_count):
        raise ValueError(
            f"scores shape must be (1,1,{candidate_count}), got {scores.shape}"
        )
    if boxes.dtype != scores.dtype:
        raise TypeError(
            f"boxes and scores dtypes must match, got {boxes.dtype} and {scores.dtype}"
        )
    return np.concatenate([boxes, scores], axis=1)


def assemble_six_head_logits(
    outputs: Sequence[np.ndarray],
) -> tuple[np.ndarray, np.ndarray]:
    """Flatten and assemble final Conv heads in s8, s16, s32 order."""

    expected = (
        ("regression_s8", (1, 64, 40, 40)),
        ("class_s8", (1, 1, 40, 40)),
        ("regression_s16", (1, 64, 20, 20)),
        ("class_s16", (1, 1, 20, 20)),
        ("regression_s32", (1, 64, 10, 10)),
        ("class_s32", (1, 1, 10, 10)),
    )
    if len(outputs) != len(expected):
        raise ValueError(f"expected six final-Conv outputs, got {len(outputs)}")

    flattened: list[np.ndarray] = []
    for output, (name, shape) in zip(outputs, expected):
        if output.shape != shape:
            raise ValueError(f"{name} shape must be {shape}, got {output.shape}")
        if output.dtype != np.float32:
            raise TypeError(f"{name} dtype must be float32, got {output.dtype}")
        flattened.append(output.reshape(1, shape[1], shape[2] * shape[3]))

    regression_logits = np.concatenate(flattened[0::2], axis=2)
    class_logits = np.concatenate(flattened[1::2], axis=2)
    return regression_logits, class_logits


def _sigmoid_float32(values: np.ndarray) -> np.ndarray:
    result = np.empty_like(values, dtype=np.float32)
    nonnegative = values >= np.float32(0.0)
    result[nonnegative] = np.float32(1.0) / (
        np.float32(1.0) + np.exp(-values[nonnegative])
    )
    negative_exp = np.exp(values[~nonnegative])
    result[~nonnegative] = negative_exp / (np.float32(1.0) + negative_exp)
    return result


class ResultCapacityError(ValueError):
    """Raised when NMS produces more detections than firmware can publish."""


@dataclass(frozen=True)
class Detection:
    candidate_index: int
    confidence: float
    x1: float
    y1: float
    x2: float
    y2: float

    def to_dict(self) -> dict[str, int | float]:
        return asdict(self)


def decode_yolo11_output(
    raw_output: np.ndarray,
    *,
    confidence_threshold: float = 0.25,
    candidate_capacity: int = 300,
) -> tuple[list[Detection], dict[str, int]]:
    """Decode xywh+confidence and retain a stable bounded top-score pool."""

    if raw_output.ndim == 3 and raw_output.shape[:2] == (1, 5):
        channels = raw_output[0]
        candidate_count = raw_output.shape[2]
    elif raw_output.ndim == 2 and raw_output.shape[0] == 5:
        channels = raw_output
        candidate_count = raw_output.shape[1]
    else:
        raise ValueError(
            "raw_output shape must be (1,5,N) or (5,N), "
            f"got {raw_output.shape}"
        )
    model_size_by_candidates = {2100: 320, EXPECTED_CANDIDATES: 640}
    if candidate_count not in model_size_by_candidates:
        raise ValueError(
            f"raw_output candidate count must be 2100 or 8400, got {candidate_count}"
        )
    model_size = model_size_by_candidates[candidate_count]
    coordinate_scale = FRAME_WIDTH / model_size
    letterbox_top = model_size / 8.0
    if not 0.0 <= confidence_threshold <= 1.0:
        raise ValueError("confidence_threshold must be in [0,1]")
    if candidate_capacity <= 0:
        raise ValueError("candidate_capacity must be positive")

    heap: list[tuple[float, int, Detection]] = []
    threshold_count = 0
    valid_geometry_count = 0
    geometry_rejected = 0
    nonfinite_rejected = 0

    for candidate_index in range(candidate_count):
        confidence = float(channels[4, candidate_index])
        if not math.isfinite(confidence):
            nonfinite_rejected += 1
            continue
        if confidence < confidence_threshold:
            continue

        values = [float(channels[channel, candidate_index]) for channel in range(4)]
        if not all(math.isfinite(value) for value in values):
            nonfinite_rejected += 1
            continue
        center_x, center_y, width, height = values
        if width <= 0.0 or height <= 0.0:
            geometry_rejected += 1
            continue

        # Firmware increments this diagnostic only after the complete candidate
        # passes finite-value and positive-size validation.
        threshold_count += 1
        x1 = _clip((center_x - width / 2.0) * coordinate_scale, 0.0, FRAME_WIDTH)
        y1 = _clip(
            (center_y - height / 2.0 - letterbox_top) * coordinate_scale,
            0.0,
            FRAME_HEIGHT,
        )
        x2 = _clip((center_x + width / 2.0) * coordinate_scale, 0.0, FRAME_WIDTH)
        y2 = _clip(
            (center_y + height / 2.0 - letterbox_top) * coordinate_scale,
            0.0,
            FRAME_HEIGHT,
        )
        if x2 <= x1 or y2 <= y1:
            geometry_rejected += 1
            continue

        valid_geometry_count += 1
        detection = Detection(candidate_index, confidence, x1, y1, x2, y2)
        # Higher confidence wins; for ties, the lower source index wins.
        key = (confidence, -candidate_index)
        entry = (key[0], key[1], detection)
        if len(heap) < candidate_capacity:
            heapq.heappush(heap, entry)
        elif entry[:2] > heap[0][:2]:
            heapq.heapreplace(heap, entry)

    retained = [item[2] for item in heap]
    retained.sort(key=lambda detection: (-detection.confidence, detection.candidate_index))
    metrics = {
        "scanned": candidate_count,
        "above_threshold": threshold_count,
        "geometry_rejected": geometry_rejected,
        "nonfinite_rejected": nonfinite_rejected,
        "valid_geometry": valid_geometry_count,
        "pool_capacity": candidate_capacity,
        "pool_retained": len(retained),
        "pool_overflow": max(0, valid_geometry_count - len(retained)),
    }
    return retained, metrics


def global_nms(
    candidates: Iterable[Detection],
    *,
    iou_threshold: float = 0.45,
    result_capacity: int = 64,
) -> list[Detection]:
    """Apply NMS and reject results that exceed the firmware output capacity."""

    if not 0.0 <= iou_threshold <= 1.0:
        raise ValueError("iou_threshold must be in [0,1]")
    if result_capacity <= 0:
        raise ValueError("result_capacity must be positive")

    ordered = sorted(
        candidates,
        key=lambda detection: (-detection.confidence, detection.candidate_index),
    )
    retained: list[Detection] = []
    for candidate in ordered:
        if all(_iou(candidate, prior) <= iou_threshold for prior in retained):
            if len(retained) >= result_capacity:
                raise ResultCapacityError(
                    "NMS result exceeds firmware capacity "
                    f"of {result_capacity} detections"
                )
            retained.append(candidate)
    return retained


def _clip(value: float, lower: float, upper: float) -> float:
    return min(max(value, lower), upper)


def _iou(left: Detection, right: Detection) -> float:
    intersection_width = max(0.0, min(left.x2, right.x2) - max(left.x1, right.x1))
    intersection_height = max(0.0, min(left.y2, right.y2) - max(left.y1, right.y1))
    intersection = intersection_width * intersection_height
    left_area = (left.x2 - left.x1) * (left.y2 - left.y1)
    right_area = (right.x2 - right.x1) * (right.y2 - right.y1)
    union = left_area + right_area - intersection
    return intersection / union if union > 0.0 else 0.0
