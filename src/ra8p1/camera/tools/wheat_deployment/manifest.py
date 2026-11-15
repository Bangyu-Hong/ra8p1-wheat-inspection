"""Deterministic GWHD calibration/golden manifest and tensor preparation."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
from typing import Any

import numpy as np

from .contract import sha256_file
from .preprocess import VGA_HEIGHT, VGA_WIDTH, build_fp32_model_input


SCHEMA_VERSION = 1
DOMAIN_QUANTILE_NUMERATORS = (0, 1, 2, 3, 4)
QUANTILE_DENOMINATOR = 4
GOLDEN_QUANTILE_NUMERATORS = frozenset((0, 2, 4))


def build_gwhd_manifest(dataset_root: Path) -> dict[str, Any]:
    annotation_paths = sorted(dataset_root.glob("*.json"), key=lambda path: path.name)
    if len(annotation_paths) != 7:
        raise ValueError(f"expected 7 annotated domain JSON files, found {len(annotation_paths)}")

    annotation_files: list[dict[str, Any]] = []
    selected_by_path: dict[str, dict[str, Any]] = {}
    total_images = 0
    total_boxes = 0
    total_negative_images = 0

    for annotation_path in annotation_paths:
        domain = annotation_path.stem
        with annotation_path.open("r", encoding="utf-8") as stream:
            coco = json.load(stream)
        _validate_categories(coco, annotation_path)

        image_by_id = {image["id"]: image for image in coco["images"]}
        if len(image_by_id) != len(coco["images"]):
            raise ValueError(f"duplicate image IDs in {annotation_path}")
        box_counts = Counter(annotation["image_id"] for annotation in coco["annotations"])
        unknown_image_ids = set(box_counts) - set(image_by_id)
        if unknown_image_ids:
            raise ValueError(f"annotations reference unknown image IDs in {annotation_path}")

        ranked: list[tuple[int, str, dict[str, Any]]] = []
        negative_images = 0
        for image in image_by_id.values():
            relative_path = _normalize_relative_path(domain, image)
            box_count = box_counts[image["id"]]
            negative_images += int(box_count == 0)
            ranked.append((box_count, relative_path, image))
        ranked.sort(key=lambda item: (item[0], item[1]))

        selected_indexes = {
            numerator: numerator * (len(ranked) - 1) // QUANTILE_DENOMINATOR
            for numerator in DOMAIN_QUANTILE_NUMERATORS
        }
        for numerator, index in selected_indexes.items():
            box_count, relative_path, image = ranked[index]
            image_path = _resolve_member(dataset_root, relative_path)
            if not image_path.is_file():
                raise FileNotFoundError(image_path)
            entry = selected_by_path.setdefault(
                relative_path,
                {
                    "domain": domain,
                    "image_id": image["id"],
                    "relative_path": relative_path,
                    "bytes": image_path.stat().st_size,
                    "sha256": sha256_file(image_path),
                    "width": image["width"],
                    "height": image["height"],
                    "box_count": box_count,
                    "roles": [],
                    "selection_quantiles": [],
                },
            )
            if "calibration" not in entry["roles"]:
                entry["roles"].append("calibration")
            entry["selection_quantiles"].append(
                f"{numerator}/{QUANTILE_DENOMINATOR}"
            )
            if numerator in GOLDEN_QUANTILE_NUMERATORS and "golden" not in entry["roles"]:
                entry["roles"].append("golden")

        annotation_files.append(
            {
                "domain": domain,
                "relative_path": annotation_path.name,
                "bytes": annotation_path.stat().st_size,
                "sha256": sha256_file(annotation_path),
                "image_count": len(image_by_id),
                "box_count": len(coco["annotations"]),
                "negative_image_count": negative_images,
            }
        )
        total_images += len(image_by_id)
        total_boxes += len(coco["annotations"])
        total_negative_images += negative_images

    entries = sorted(selected_by_path.values(), key=lambda item: item["relative_path"])
    manifest: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "dataset": {
            "id": "global-wheat-head-dataset-2020-v4",
            "source": "https://doi.org/10.5281/zenodo.4298502",
            "license": "MIT",
            "annotated_domains": [item["domain"] for item in annotation_files],
            "image_count": total_images,
            "box_count": total_boxes,
            "negative_image_count": total_negative_images,
            "annotation_files": annotation_files,
        },
        "selection": {
            "algorithm": "per-domain-box-count-quantiles-v1",
            "ordering": "ascending (box_count, relative_path)",
            "quantiles": [
                f"{numerator}/{QUANTILE_DENOMINATOR}"
                for numerator in DOMAIN_QUANTILE_NUMERATORS
            ],
            "golden_quantiles": [
                f"{numerator}/{QUANTILE_DENOMINATOR}"
                for numerator in sorted(GOLDEN_QUANTILE_NUMERATORS)
            ],
            "calibration_image_count": sum(
                "calibration" in item["roles"] for item in entries
            ),
            "golden_image_count": sum("golden" in item["roles"] for item in entries),
        },
        "source_view_preprocessing": {
            "purpose": "approximate a production 4:3 camera view from square GWHD images",
            "color_order": "RGB",
            "crop": "center 4:3 crop using integer bounds",
            "resize": "Pillow bilinear to 640x480",
        },
        "model_preprocessing": {
            "input_vga_shape": [480, 640, 3],
            "letterbox_shape": [640, 640, 3],
            "letterbox_top_rows": 80,
            "letterbox_bottom_rows": 80,
            "padding_rgb": [114, 114, 114],
            "layout": "NCHW",
            "output_shape": [1, 3, 640, 640],
            "dtype": "float32",
            "normalization": "uint8 / 255.0",
        },
        "images": entries,
    }
    manifest["manifest_sha256"] = _manifest_digest(manifest)
    return manifest


def write_manifest(manifest: dict[str, Any], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def verify_gwhd_manifest(dataset_root: Path, manifest: dict[str, Any]) -> dict[str, int]:
    expected_digest = manifest.get("manifest_sha256")
    actual_digest = _manifest_digest(manifest)
    if expected_digest != actual_digest:
        raise ValueError(
            f"manifest hash mismatch expected={expected_digest} actual={actual_digest}"
        )

    annotation_count = 0
    for item in manifest["dataset"]["annotation_files"]:
        path = _resolve_member(dataset_root, item["relative_path"])
        _verify_file(path, item)
        annotation_count += 1

    image_count = 0
    for item in manifest["images"]:
        path = _resolve_member(dataset_root, item["relative_path"])
        _verify_file(path, item)
        image_count += 1
    return {"annotation_files": annotation_count, "images": image_count}


def prepare_calibration_tensors(
    dataset_root: Path,
    manifest: dict[str, Any],
    output_dir: Path,
    *,
    model_size: int = 640,
) -> dict[str, Any]:
    """Write one deterministic float32 NCHW tensor per selected image."""

    verify_gwhd_manifest(dataset_root, manifest)
    output_dir.mkdir(parents=True, exist_ok=True)
    output_entries: list[dict[str, Any]] = []

    for item in manifest["images"]:
        if "calibration" not in item["roles"]:
            continue
        source_path = _resolve_member(dataset_root, item["relative_path"])
        vga_rgb = load_gwhd_vga_rgb(source_path)
        tensor = build_fp32_model_input(vga_rgb, model_size=model_size)
        output_name = item["relative_path"].replace("/", "__").removesuffix(".png") + ".npy"
        output_path = output_dir / output_name
        np.save(output_path, tensor, allow_pickle=False)
        output_entries.append(
            {
                "source_relative_path": item["relative_path"],
                "source_sha256": item["sha256"],
                "tensor_file": output_name,
                "tensor_file_sha256": sha256_file(output_path),
                "tensor_bytes_sha256": hashlib.sha256(tensor.tobytes(order="C")).hexdigest(),
                "shape": list(tensor.shape),
                "dtype": str(tensor.dtype),
                "minimum": float(tensor.min()),
                "maximum": float(tensor.max()),
            }
        )

    index: dict[str, Any] = {
        "schema_version": 1,
        "source_manifest_sha256": manifest["manifest_sha256"],
        "preprocessing": {
            "camera_color_order": "RGB",
            "camera_shape": [480, 640, 3],
            "model_size": model_size,
            "normalization": "uint8 / 255.0",
            "padding_rgb": [114, 114, 114],
            "resize": (
                "none"
                if model_size == 640
                else "cv2.INTER_LINEAR 640x480 to 320x240"
            ),
            "top_bottom_padding_rows": (640 - 480) // 2 if model_size == 640 else 40,
        },
        "tensor_count": len(output_entries),
        "entries": output_entries,
    }
    index["index_sha256"] = _manifest_digest(index, digest_field="index_sha256")
    write_manifest(index, output_dir / "calibration_index.json")
    return index


def load_gwhd_vga_rgb(source_path: Path) -> np.ndarray:
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("Pillow is required to prepare GWHD calibration images") from exc

    with Image.open(source_path) as image:
        rgb = image.convert("RGB")
        width, height = rgb.size
        crop_width = min(width, height * 4 // 3)
        crop_height = min(height, width * 3 // 4)
        left = (width - crop_width) // 2
        top = (height - crop_height) // 2
        cropped = rgb.crop((left, top, left + crop_width, top + crop_height))
        resized = cropped.resize((VGA_WIDTH, VGA_HEIGHT), Image.Resampling.BILINEAR)
        return np.asarray(resized, dtype=np.uint8).copy()


def _validate_categories(coco: dict[str, Any], annotation_path: Path) -> None:
    categories = coco.get("categories")
    if not isinstance(categories, list) or len(categories) != 1:
        raise ValueError(f"expected one category in {annotation_path}")
    if categories[0].get("id") != 1 or categories[0].get("name") != "wheat_head":
        raise ValueError(f"unexpected category contract in {annotation_path}")


def _normalize_relative_path(domain: str, image: dict[str, Any]) -> str:
    raw_path = image.get("path") or f"{domain}/{image['file_name']}"
    normalized = PurePosixPath(str(raw_path).replace("\\", "/"))
    if normalized.is_absolute() or ".." in normalized.parts:
        raise ValueError(f"unsafe dataset relative path: {raw_path}")
    if normalized.parts[0] != domain:
        raise ValueError(f"image path {raw_path} does not belong to domain {domain}")
    return normalized.as_posix()


def _resolve_member(root: Path, relative_path: str) -> Path:
    relative = PurePosixPath(relative_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"unsafe relative path: {relative_path}")
    return root.joinpath(*relative.parts)


def _verify_file(path: Path, expected: dict[str, Any]) -> None:
    if not path.is_file():
        raise FileNotFoundError(path)
    actual_bytes = path.stat().st_size
    if actual_bytes != expected["bytes"]:
        raise ValueError(
            f"size mismatch for {path}: expected={expected['bytes']} actual={actual_bytes}"
        )
    actual_sha256 = sha256_file(path)
    if actual_sha256 != expected["sha256"]:
        raise ValueError(
            f"sha256 mismatch for {path}: expected={expected['sha256']} actual={actual_sha256}"
        )


def _manifest_digest(manifest: dict[str, Any], digest_field: str = "manifest_sha256") -> str:
    digest_input = dict(manifest)
    digest_input.pop(digest_field, None)
    canonical = json.dumps(
        digest_input,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()
