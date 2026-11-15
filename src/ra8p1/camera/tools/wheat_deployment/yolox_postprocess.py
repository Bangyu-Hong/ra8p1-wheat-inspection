"""YOLOX-Tiny raw-head decode and class-agnostic NMS reference."""

from __future__ import annotations

from dataclasses import dataclass
import math

import numpy as np


MODEL_SIZE = 224
STRIDES = (8, 16, 32)
EXPECTED_CANDIDATES = 1029


@dataclass(frozen=True)
class YoloxDetection:
    candidate_index: int
    confidence: float
    x1: float
    y1: float
    x2: float
    y2: float


def decode_yolox_raw(
    raw_output: np.ndarray,
    *,
    confidence_threshold: float = 0.25,
) -> list[YoloxDetection]:
    """Decode YOLOX raw `[tx,ty,tw,th,obj,cls]` rows to model-space boxes."""

    if raw_output.shape != (1, EXPECTED_CANDIDATES, 6):
        raise ValueError(
            "YOLOX raw output must have shape (1,1029,6), "
            f"got {raw_output.shape}"
        )
    if raw_output.dtype != np.float32:
        raise TypeError(f"YOLOX raw output must be float32, got {raw_output.dtype}")
    if not 0.0 <= confidence_threshold <= 1.0:
        raise ValueError("confidence_threshold must be in [0,1]")

    grids: list[np.ndarray] = []
    expanded_strides: list[np.ndarray] = []
    for stride in STRIDES:
        size = MODEL_SIZE // stride
        grid_y, grid_x = np.meshgrid(
            np.arange(size, dtype=np.float32),
            np.arange(size, dtype=np.float32),
            indexing="ij",
        )
        grids.append(np.stack((grid_x, grid_y), axis=-1).reshape(-1, 2))
        expanded_strides.append(
            np.full((size * size, 1), stride, dtype=np.float32)
        )
    grid = np.concatenate(grids, axis=0)
    strides = np.concatenate(expanded_strides, axis=0)

    rows = raw_output[0]
    centers = (rows[:, :2] + grid) * strides
    sizes = np.exp(rows[:, 2:4]).astype(np.float32) * strides
    objectness = rows[:, 4]
    class_probability = rows[:, 5]
    valid_probabilities = (objectness > 0.0) & (class_probability > 0.0)
    # A tensor-wide INT8 scale can dequantize sigmoid channels slightly above
    # one or below zero.  Match the MCU boundary: reject invalid negative
    # probabilities and saturate small positive overshoot before multiplying.
    scores = np.where(
        valid_probabilities,
        np.minimum(objectness, 1.0) * np.minimum(class_probability, 1.0),
        -np.inf,
    )
    detections: list[YoloxDetection] = []
    for candidate_index in np.flatnonzero(scores >= confidence_threshold):
        center_x, center_y = centers[candidate_index]
        width, height = sizes[candidate_index]
        confidence = float(scores[candidate_index])
        values = (center_x, center_y, width, height, confidence)
        if not all(math.isfinite(float(value)) for value in values):
            continue
        if width <= 0.0 or height <= 0.0:
            continue
        detections.append(
            YoloxDetection(
                candidate_index=int(candidate_index),
                confidence=confidence,
                x1=float(center_x - width / 2.0),
                y1=float(center_y - height / 2.0),
                x2=float(center_x + width / 2.0),
                y2=float(center_y + height / 2.0),
            )
        )
    return sorted(
        detections,
        key=lambda item: (-item.confidence, item.candidate_index),
    )


def yolox_class_agnostic_nms(
    candidates: list[YoloxDetection],
    *,
    iou_threshold: float = 0.45,
) -> list[YoloxDetection]:
    if not 0.0 <= iou_threshold <= 1.0:
        raise ValueError("iou_threshold must be in [0,1]")
    retained: list[YoloxDetection] = []
    for candidate in sorted(
        candidates,
        key=lambda item: (-item.confidence, item.candidate_index),
    ):
        if all(_iou(candidate, prior) <= iou_threshold for prior in retained):
            retained.append(candidate)
    return retained


def _iou(left: YoloxDetection, right: YoloxDetection) -> float:
    intersection_width = max(0.0, min(left.x2, right.x2) - max(left.x1, right.x1))
    intersection_height = max(0.0, min(left.y2, right.y2) - max(left.y1, right.y1))
    intersection = intersection_width * intersection_height
    left_area = (left.x2 - left.x1) * (left.y2 - left.y1)
    right_area = (right.x2 - right.x1) * (right.y2 - right.y1)
    union = left_area + right_area - intersection
    return intersection / union if union > 0.0 else 0.0
