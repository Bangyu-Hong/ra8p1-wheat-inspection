"""Prepare deterministic YOLOX full224 calibration tensors from its handoff."""

from __future__ import annotations

from collections import defaultdict
import csv
from dataclasses import dataclass
import hashlib
from io import BytesIO, StringIO
import json
from pathlib import Path, PurePosixPath
from typing import Any, Iterable
from zipfile import ZipFile

import numpy as np

from .contract import sha256_file


PURPOSE = "full224-chain-smoke"
MODEL_SIZE = 224
PADDING_VALUE = 114
DENSITY_LEVELS = 5
SELECTION_SEED = "ra8p1-yolox-full224-calibration-v1"
REQUIRED_COLUMNS = frozenset(
    {
        "split",
        "source_member",
        "output_image",
        "domain",
        "source_image_id",
        "crop_x",
        "crop_y",
        "width",
        "height",
        "source_boxes",
        "output_sha256",
    }
)


@dataclass(frozen=True)
class CalibrationRow:
    split: str
    source_member: str
    output_image: str
    domain: str
    source_image_id: int
    crop_x: int
    crop_y: int
    width: int
    height: int
    source_boxes: int
    output_sha256: str
    density_level: int = -1

    @property
    def tensor_stem(self) -> str:
        return f"{self.domain}__{PurePosixPath(self.source_member).stem}"


def load_split_manifest(path: Path) -> list[CalibrationRow]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        columns = frozenset(reader.fieldnames or ())
        missing = sorted(REQUIRED_COLUMNS - columns)
        if missing:
            raise ValueError(f"split manifest is missing columns: {missing}")
        rows = [_parse_row(raw, index + 2) for index, raw in enumerate(reader)]
    if not rows:
        raise ValueError("split manifest contains no rows")
    return rows


def select_calibration_rows(
    rows: Iterable[CalibrationRow],
    *,
    count: int = 200,
    expected_domains: int = 7,
) -> list[CalibrationRow]:
    train_by_domain: dict[str, list[CalibrationRow]] = defaultdict(list)
    for row in rows:
        if row.split == "train":
            train_by_domain[row.domain].append(row)
    if len(train_by_domain) != expected_domains:
        raise ValueError(
            f"expected {expected_domains} train domains, found {len(train_by_domain)}"
        )

    strata: dict[tuple[str, int], list[CalibrationRow]] = {}
    for domain, domain_rows in sorted(train_by_domain.items()):
        ranked = sorted(
            domain_rows,
            key=lambda item: (item.source_boxes, item.source_member),
        )
        for rank, row in enumerate(ranked):
            level = min(DENSITY_LEVELS - 1, rank * DENSITY_LEVELS // len(ranked))
            strata.setdefault((domain, level), []).append(
                CalibrationRow(**{**row.__dict__, "density_level": level})
            )

    minimum = len(strata)
    available = sum(len(items) for items in strata.values())
    if count < minimum:
        raise ValueError(
            f"count {count} cannot cover all {minimum} domain-density strata"
        )
    if count > available:
        raise ValueError(f"requested {count} rows but only {available} are available")

    quotas = _allocate_quotas(strata, count)
    selected: list[CalibrationRow] = []
    for key in sorted(strata):
        ordered = sorted(strata[key], key=_selection_key)
        selected.extend(ordered[: quotas[key]])
    return sorted(selected, key=lambda item: (item.domain, item.density_level, item.source_member))


def reconstruct_yolox_input(dataset_root: Path, row: CalibrationRow) -> tuple[np.ndarray, bytes]:
    cv2 = _load_cv2()
    source_path = _resolve_source_member(dataset_root, row.source_member)
    source = cv2.imdecode(
        np.frombuffer(source_path.read_bytes(), dtype=np.uint8),
        cv2.IMREAD_COLOR,
    )
    if source is None:
        raise ValueError(f"OpenCV could not decode source image: {source_path}")
    source_height, source_width = source.shape[:2]
    x1 = row.crop_x + row.width
    y1 = row.crop_y + row.height
    if row.crop_x < 0 or row.crop_y < 0 or x1 > source_width or y1 > source_height:
        raise ValueError(
            f"crop is outside source image {row.source_member}: "
            f"crop=({row.crop_x},{row.crop_y},{row.width},{row.height}) "
            f"source=({source_width},{source_height})"
        )
    cropped = source[row.crop_y:y1, row.crop_x:x1]
    encoded_ok, encoded = cv2.imencode(
        ".jpg",
        cropped,
        [cv2.IMWRITE_JPEG_QUALITY, 95],
    )
    if not encoded_ok:
        raise RuntimeError(f"OpenCV could not encode crop: {row.source_member}")
    jpeg_bytes = encoded.tobytes()
    jpeg_sha256 = hashlib.sha256(jpeg_bytes).hexdigest()
    if jpeg_sha256 != row.output_sha256:
        raise ValueError(
            f"reconstructed JPEG hash mismatch for {row.source_member}: "
            f"expected {row.output_sha256}, observed {jpeg_sha256}"
        )

    decoded = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
    if decoded is None:
        raise RuntimeError(f"OpenCV could not decode reconstructed JPEG: {row.source_member}")
    rgb = cv2.cvtColor(decoded, cv2.COLOR_BGR2RGB)
    scale = min(MODEL_SIZE / rgb.shape[0], MODEL_SIZE / rgb.shape[1])
    resized_width = int(rgb.shape[1] * scale)
    resized_height = int(rgb.shape[0] * scale)
    resized = cv2.resize(
        rgb,
        (resized_width, resized_height),
        interpolation=cv2.INTER_LINEAR,
    ).astype(np.uint8)
    padded = np.full(
        (MODEL_SIZE, MODEL_SIZE, 3),
        PADDING_VALUE,
        dtype=np.uint8,
    )
    padded[:resized_height, :resized_width] = resized
    tensor = np.ascontiguousarray(padded.transpose(2, 0, 1), dtype=np.float32)[
        np.newaxis, ...
    ]
    return tensor, jpeg_bytes


def verify_golden_inputs(
    dataset_root: Path,
    rows: Iterable[CalibrationRow],
    golden_zip: Path,
) -> dict[str, Any]:
    rows_by_file = {PurePosixPath(row.output_image).name: row for row in rows}
    cases: list[dict[str, Any]] = []
    global_max_difference = 0.0
    with ZipFile(golden_zip) as archive:
        manifest_rows = list(
            csv.DictReader(
                StringIO(archive.read("manifest.csv").decode("utf-8-sig"))
            )
        )
        if len(manifest_rows) != 20:
            raise ValueError(f"expected 20 golden inputs, found {len(manifest_rows)}")
        for golden in manifest_rows:
            file_name = golden["file_name"]
            row = rows_by_file.get(file_name)
            if row is None:
                raise ValueError(f"golden image is absent from split manifest: {file_name}")
            member_name = f"inputs/{Path(file_name).stem}.npy"
            golden_bytes = archive.read(member_name)
            golden_sha256 = hashlib.sha256(golden_bytes).hexdigest()
            if golden_sha256 != golden["input_sha256"]:
                raise ValueError(
                    f"golden member hash mismatch for {file_name}: "
                    f"expected {golden['input_sha256']}, observed {golden_sha256}"
                )
            expected = np.load(BytesIO(golden_bytes), allow_pickle=False)
            observed, _ = reconstruct_yolox_input(dataset_root, row)
            if expected.shape != observed.shape or expected.dtype != observed.dtype:
                raise ValueError(
                    f"golden tensor contract mismatch for {file_name}: "
                    f"expected {expected.dtype} {expected.shape}, "
                    f"observed {observed.dtype} {observed.shape}"
                )
            difference = float(np.max(np.abs(expected - observed)))
            global_max_difference = max(global_max_difference, difference)
            if not np.array_equal(expected, observed):
                raise ValueError(
                    f"golden preprocessing mismatch for {file_name}: "
                    f"maximum difference {difference}"
                )
            cases.append(
                {
                    "file_name": file_name,
                    "source_member": row.source_member,
                    "input_sha256": golden_sha256,
                    "maximum_difference": difference,
                }
            )
    return {
        "case_count": len(cases),
        "all_exact": True,
        "maximum_difference": global_max_difference,
        "cases": cases,
    }


def prepare_yolox_calibration(
    dataset_root: Path,
    split_manifest: Path,
    golden_zip: Path,
    output_dir: Path,
    *,
    count: int = 200,
) -> dict[str, Any]:
    rows = load_split_manifest(split_manifest)
    golden_result = verify_golden_inputs(dataset_root, rows, golden_zip)
    selected = select_calibration_rows(rows, count=count)

    if output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError(f"output directory is not empty: {output_dir}")
    tensor_dir = output_dir / "tensors"
    tensor_dir.mkdir(parents=True, exist_ok=True)

    entries: list[dict[str, Any]] = []
    for index, row in enumerate(selected):
        source_path = _resolve_source_member(dataset_root, row.source_member)
        tensor, jpeg_bytes = reconstruct_yolox_input(dataset_root, row)
        tensor_name = f"{index:03d}_{row.tensor_stem}.npy"
        tensor_path = tensor_dir / tensor_name
        np.save(tensor_path, tensor, allow_pickle=False)
        entries.append(
            {
                "index": index,
                "split": row.split,
                "domain": row.domain,
                "density_level": row.density_level,
                "source_boxes": row.source_boxes,
                "source_image_id": row.source_image_id,
                "source_member": row.source_member,
                "source_bytes": source_path.stat().st_size,
                "source_sha256": sha256_file(source_path),
                "crop": {
                    "x": row.crop_x,
                    "y": row.crop_y,
                    "width": row.width,
                    "height": row.height,
                },
                "training_jpeg": PurePosixPath(row.output_image).as_posix(),
                "training_jpeg_sha256": hashlib.sha256(jpeg_bytes).hexdigest(),
                "tensor_file": f"tensors/{tensor_name}",
                "tensor_sha256": sha256_file(tensor_path),
                "tensor_dtype": str(tensor.dtype),
                "tensor_shape": list(tensor.shape),
                "tensor_range": [float(tensor.min()), float(tensor.max())],
            }
        )

    stratum_counts: dict[str, int] = defaultdict(int)
    for entry in entries:
        stratum_counts[f"{entry['domain']}:density-{entry['density_level']}"] += 1
    manifest = {
        "schema_version": 1,
        "purpose": PURPOSE,
        "qualification_scope": {
            "proves": "YOLOX full224 preprocessing, INT8, MERA, and Ethos-U55 chain compatibility",
            "does_not_prove": [
                "tile224/stride179 VGA accuracy",
                "12-tile local-to-global box restoration",
                "global-NMS counting accuracy",
                "competition model acceptance",
            ],
            "final_tile_model": False,
        },
        "source": {
            "split_manifest": str(split_manifest.resolve()),
            "split_manifest_sha256": sha256_file(split_manifest),
            "golden_zip": str(golden_zip.resolve()),
            "golden_zip_sha256": sha256_file(golden_zip),
        },
        "selection": {
            "algorithm": "train-only-domain-density-quintile-hash-v1",
            "seed": SELECTION_SEED,
            "requested_count": count,
            "selected_count": len(entries),
            "domain_count": len({entry["domain"] for entry in entries}),
            "density_level_count": DENSITY_LEVELS,
            "stratum_counts": dict(sorted(stratum_counts.items())),
        },
        "preprocessing": {
            "source_crop": "split-manifest crop from original GWHD PNG",
            "intermediate": "OpenCV JPEG quality 95 encode/decode",
            "color": "BGR to RGB",
            "resize": "OpenCV INTER_LINEAR, aspect ratio preserved",
            "resized_shape_hwc": [168, 224, 3],
            "placement": "top-left",
            "padding": {"value": PADDING_VALUE, "bottom_rows": 56},
            "tensor": {"dtype": "float32", "shape": [1, 3, 224, 224], "range": [0, 255]},
        },
        "golden_validation": golden_result,
        "entries": entries,
    }
    manifest_path = output_dir / "calibration_manifest.json"
    with manifest_path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return {
        "purpose": PURPOSE,
        "tensor_count": len(entries),
        "manifest": str(manifest_path),
        "manifest_sha256": sha256_file(manifest_path),
        "golden_case_count": golden_result["case_count"],
        "golden_maximum_difference": golden_result["maximum_difference"],
    }


def _parse_row(raw: dict[str, str], line_number: int) -> CalibrationRow:
    try:
        row = CalibrationRow(
            split=raw["split"],
            source_member=raw["source_member"],
            output_image=raw["output_image"],
            domain=raw["domain"],
            source_image_id=int(raw["source_image_id"]),
            crop_x=int(raw["crop_x"]),
            crop_y=int(raw["crop_y"]),
            width=int(raw["width"]),
            height=int(raw["height"]),
            source_boxes=int(raw["source_boxes"]),
            output_sha256=raw["output_sha256"].lower(),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid split manifest row at line {line_number}") from exc
    if row.split not in {"train", "val", "test"}:
        raise ValueError(f"invalid split at line {line_number}: {row.split}")
    if PurePosixPath(row.source_member).is_absolute() or ".." in PurePosixPath(row.source_member).parts:
        raise ValueError(f"unsafe source member at line {line_number}: {row.source_member}")
    if row.width <= 0 or row.height <= 0 or row.source_boxes < 0:
        raise ValueError(f"invalid geometry/count at line {line_number}")
    if len(row.output_sha256) != 64:
        raise ValueError(f"invalid output SHA-256 at line {line_number}")
    return row


def _allocate_quotas(
    strata: dict[tuple[str, int], list[CalibrationRow]], count: int
) -> dict[tuple[str, int], int]:
    quotas = {key: 1 for key in strata}
    remaining = count - len(strata)
    capacities = {key: len(items) - 1 for key, items in strata.items()}
    total_capacity = sum(capacities.values())
    exact = {
        key: (remaining * capacity / total_capacity if total_capacity else 0.0)
        for key, capacity in capacities.items()
    }
    for key in quotas:
        quotas[key] += int(exact[key])
    unassigned = count - sum(quotas.values())
    remainder_order = sorted(
        strata,
        key=lambda key: (-(exact[key] - int(exact[key])), key),
    )
    for key in remainder_order[:unassigned]:
        quotas[key] += 1
    if sum(quotas.values()) != count:
        raise RuntimeError("internal calibration quota allocation failure")
    for key, quota in quotas.items():
        if quota > len(strata[key]):
            raise RuntimeError(f"quota exceeds stratum capacity: {key}")
    return quotas


def _selection_key(row: CalibrationRow) -> tuple[str, str]:
    digest = hashlib.sha256(
        f"{SELECTION_SEED}\0{row.source_member}".encode("utf-8")
    ).hexdigest()
    return digest, row.source_member


def _resolve_source_member(dataset_root: Path, member: str) -> Path:
    member_path = PurePosixPath(member)
    path = dataset_root.joinpath(*member_path.parts)
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def _load_cv2():
    try:
        import cv2
    except ImportError as exc:
        raise RuntimeError("opencv-python is required for YOLOX calibration") from exc
    return cv2
