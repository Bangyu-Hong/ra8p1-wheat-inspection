#!/usr/bin/env python3
"""Validate an RGB565 frame dump and convert it to binary PPM."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import zlib


DEFAULT_WIDTH = 640
DEFAULT_HEIGHT = 480
BYTES_PER_PIXEL = 2


def rgb565_to_rgb888(raw: bytes, width: int, height: int, byte_order: str) -> bytes:
    """Convert packed RGB565 bytes into packed RGB888 bytes."""
    rgb = bytearray(width * height * 3)
    output_index = 0

    for input_index in range(0, len(raw), BYTES_PER_PIXEL):
        first = raw[input_index]
        second = raw[input_index + 1]
        if byte_order == "big":
            pixel = (first << 8) | second
        else:
            pixel = first | (second << 8)

        red_5 = (pixel >> 11) & 0x1F
        green_6 = (pixel >> 5) & 0x3F
        blue_5 = pixel & 0x1F
        rgb[output_index] = (red_5 << 3) | (red_5 >> 2)
        rgb[output_index + 1] = (green_6 << 2) | (green_6 >> 4)
        rgb[output_index + 2] = (blue_5 << 3) | (blue_5 >> 2)
        output_index += 3

    return bytes(rgb)


def write_ppm(output_path: Path, rgb: bytes, width: int, height: int) -> None:
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    output_path.write_bytes(header + rgb)


def positive_dimension(value: str) -> int:
    dimension = int(value)
    if dimension <= 0:
        raise argparse.ArgumentTypeError("dimension must be positive")
    return dimension


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate and convert one packed RGB565 CEU dump."
    )
    parser.add_argument("input", type=Path, help="raw packed RGB565 frame dump")
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        help="output binary PPM path (default: input path with .ppm suffix)",
    )
    parser.add_argument(
        "--width",
        type=positive_dimension,
        default=DEFAULT_WIDTH,
        help=f"frame width in pixels (default: {DEFAULT_WIDTH})",
    )
    parser.add_argument(
        "--height",
        type=positive_dimension,
        default=DEFAULT_HEIGHT,
        help=f"frame height in pixels (default: {DEFAULT_HEIGHT})",
    )
    parser.add_argument(
        "--byte-order",
        choices=("big", "little"),
        default="big",
        help="byte order of each RGB565 word in the raw stream (default: big)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_path = args.output or args.input.with_suffix(".ppm")
    frame_bytes = args.width * args.height * BYTES_PER_PIXEL

    try:
        raw = args.input.read_bytes()
    except OSError as error:
        print(f"error: cannot read {args.input}: {error}", file=sys.stderr)
        return 2

    if len(raw) != frame_bytes:
        print(
            f"error: expected {frame_bytes} bytes for {args.width}x{args.height}, "
            f"got {len(raw)} bytes",
            file=sys.stderr,
        )
        return 2

    crc32 = zlib.crc32(raw) & 0xFFFFFFFF
    rgb = rgb565_to_rgb888(raw, args.width, args.height, args.byte_order)

    try:
        write_ppm(output_path, rgb, args.width, args.height)
    except OSError as error:
        print(f"error: cannot write {output_path}: {error}", file=sys.stderr)
        return 2

    print(
        f"input={args.input} bytes={len(raw)} width={args.width} height={args.height} "
        f"format=RGB565 byte_order={args.byte_order} crc32=0x{crc32:08X} "
        f"output={output_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
