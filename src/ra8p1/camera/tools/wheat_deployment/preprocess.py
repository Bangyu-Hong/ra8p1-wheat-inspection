"""Scalar host reference for the production camera-to-model input contract."""

from __future__ import annotations

from typing import Any

import numpy as np


VGA_WIDTH = 640
VGA_HEIGHT = 480
MODEL_SIZE = 640
LETTERBOX_TOP = 80
PADDING_RGB = 114


def rgb565_be_to_rgb888(frame: bytes | bytearray | memoryview, stride: int = 1280) -> np.ndarray:
    """Decode one 640x480 RGB565 big-endian camera frame into uint8 RGB."""

    if stride < VGA_WIDTH * 2:
        raise ValueError(f"stride must be at least {VGA_WIDTH * 2}, got {stride}")
    expected_bytes = stride * VGA_HEIGHT
    if len(frame) != expected_bytes:
        raise ValueError(f"frame length must be {expected_bytes}, got {len(frame)}")

    rows = np.frombuffer(frame, dtype=np.uint8).reshape(VGA_HEIGHT, stride)
    packed_bytes = rows[:, : VGA_WIDTH * 2].reshape(VGA_HEIGHT, VGA_WIDTH, 2)
    packed = (packed_bytes[:, :, 0].astype(np.uint16) << 8) | packed_bytes[:, :, 1]
    red5 = (packed >> 11) & 0x1F
    green6 = (packed >> 5) & 0x3F
    blue5 = packed & 0x1F

    rgb = np.empty((VGA_HEIGHT, VGA_WIDTH, 3), dtype=np.uint8)
    rgb[:, :, 0] = ((red5 << 3) | (red5 >> 2)).astype(np.uint8)
    rgb[:, :, 1] = ((green6 << 2) | (green6 >> 4)).astype(np.uint8)
    rgb[:, :, 2] = ((blue5 << 3) | (blue5 >> 2)).astype(np.uint8)
    return rgb


def letterbox_vga_rgb(
    rgb: np.ndarray,
    padding_rgb: int = PADDING_RGB,
    *,
    model_size: int = MODEL_SIZE,
) -> np.ndarray:
    """Resize VGA RGB when required, then center-pad to a square model input."""

    if rgb.dtype != np.uint8:
        raise TypeError(f"rgb dtype must be uint8, got {rgb.dtype}")
    if rgb.shape != (VGA_HEIGHT, VGA_WIDTH, 3):
        raise ValueError(
            f"rgb shape must be {(VGA_HEIGHT, VGA_WIDTH, 3)}, got {rgb.shape}"
        )
    if not 0 <= padding_rgb <= 255:
        raise ValueError(f"padding_rgb must be in [0,255], got {padding_rgb}")
    if model_size not in (320, 640):
        raise ValueError("model_size must be 320 or 640")

    if model_size == MODEL_SIZE:
        resized = rgb
    else:
        try:
            import cv2
        except ImportError as exc:
            raise RuntimeError("OpenCV is required for full320 preprocessing") from exc
        resized = cv2.resize(
            rgb,
            (model_size, model_size * VGA_HEIGHT // VGA_WIDTH),
            interpolation=cv2.INTER_LINEAR,
        )
    letterbox_top = (model_size - resized.shape[0]) // 2
    output = np.full((model_size, model_size, 3), padding_rgb, dtype=np.uint8)
    output[letterbox_top : letterbox_top + resized.shape[0], :, :] = resized
    return output


def build_fp32_model_input(
    rgb: np.ndarray,
    *,
    model_size: int = MODEL_SIZE,
) -> np.ndarray:
    """Build the source ONNX NCHW float32 RGB input in the [0,1] range."""

    letterboxed = letterbox_vga_rgb(rgb, model_size=model_size)
    return np.ascontiguousarray(
        letterboxed.transpose(2, 0, 1)[np.newaxis, ...].astype(np.float32) / 255.0
    )


def quantize_model_input(
    fp32_input: np.ndarray,
    *,
    scale: float,
    zero_point: int,
    dtype: Any = np.int8,
) -> np.ndarray:
    """Quantize an already-normalized tensor using generated RUHMI parameters."""

    if fp32_input.shape != (1, 3, MODEL_SIZE, MODEL_SIZE):
        raise ValueError(
            f"fp32_input shape must be {(1, 3, MODEL_SIZE, MODEL_SIZE)}, "
            f"got {fp32_input.shape}"
        )
    if fp32_input.dtype != np.float32:
        raise TypeError(f"fp32_input dtype must be float32, got {fp32_input.dtype}")
    if not np.isfinite(scale) or scale <= 0.0:
        raise ValueError(f"scale must be finite and positive, got {scale}")

    output_dtype = np.dtype(dtype)
    if output_dtype not in (np.dtype(np.int8), np.dtype(np.uint8)):
        raise TypeError(f"dtype must be int8 or uint8, got {output_dtype}")
    limits = np.iinfo(output_dtype)
    transformed = (fp32_input.astype(np.float64) / scale) + zero_point
    bounded = np.clip(transformed, limits.min, limits.max)
    # Match the firmware cast-after-+/-0.5 rule at exact half-way values.
    rounded = np.where(bounded >= 0.0, np.floor(bounded + 0.5), np.ceil(bounded - 0.5))
    return rounded.astype(output_dtype)
