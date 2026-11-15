"""Fail-closed host preflight for the frozen YOLOX-Tiny tile224 candidate.

This module deliberately stops before RUHMI/MERA compilation.  It proves that
the immutable handoff, the actual TFLite ABI, the pinned Windows toolchain, and
the ASCII staging copy all match the reviewed deployment contract.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
from typing import Any, Callable

import numpy as np


EXPECTED_PROVENANCE_ENTRIES = 1879
EXPECTED_MODEL_RELATIVE_PATH = Path("models/yolox_tiny_gwhd_224_int8.tflite")
EXPECTED_MODEL_SHA256 = (
    "7f6a058eb5b4cc4cc61942cdac55a6645fbffe9992731f1f6d72d897338f07f3"
)
EXPECTED_SPLIT_SHA256 = (
    "e60a54fd1a8c9d3e0aecb5fe2c94bf1ad4e00db645ffdef2462f4f43fed79262"
)
EXPECTED_INPUT_SHAPE = [1, 224, 224, 3]
EXPECTED_OUTPUT_SHAPE = [1, 1029, 6]
EXPECTED_INPUT_QUANTIZATION = (1.0, -128)
EXPECTED_OUTPUT_QUANTIZATION = (0.03774220868945122, -48)
EXPECTED_TILE_X = [0, 179, 358, 416]
EXPECTED_TILE_Y = [0, 179, 256]
EXPECTED_CONFIDENCE = 0.25
EXPECTED_NMS_IOU = 0.45
EXPECTED_MATCH_IOU = 0.50
EXPECTED_RUHMI_HEAD = "6c5aad901a1a41e28f6e306bfc35c44659e89502"
EXPECTED_RUHMI_TAG = "Release-2026-06-19"
EXPECTED_MERA_VERSION = "2.6.0+pkg.4815"
EXPECTED_VELA_VERSION = "4.2.0"
EXPECTED_BOARD_SDRAM_BYTES = 16 * 1024 * 1024
EXPECTED_BOARD_OSPI_BYTES = 32 * 1024 * 1024
EXPECTED_CPU_FOCUS_REGIONS = 4
EXPECTED_CPU_FOCUS_OPERATOR = "TFLiteStridedSlice"
EXPECTED_NPU_REGION_COUNT = 1
_NON_EXECUTING_REGION_OPERATORS = {
    "FloatVecConstant",
    "Int32VecConstant",
    "OutputNode",
    "Var",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_provenance(
    package_root: Path,
    *,
    expected_entries: int = EXPECTED_PROVENANCE_ENTRIES,
) -> dict[str, Any]:
    """Verify every declared handoff member without modifying the package."""

    package_root = package_root.resolve()
    failures: list[str] = []
    verified = 0
    verified_bytes = 0
    provenance_path = package_root / "provenance.json"
    try:
        provenance = _load_json(provenance_path)
    except Exception as exc:  # report a stable category instead of escaping
        return {
            "status": "failed",
            "provenance": str(provenance_path),
            "expected_entries": expected_entries,
            "declared_entries": 0,
            "verified_entries": 0,
            "verified_bytes": 0,
            "failures": [f"provenance unreadable: {type(exc).__name__}: {exc}"],
        }

    entries = provenance.get("files")
    if not isinstance(entries, list):
        entries = []
        failures.append("provenance files field is not a list")
    if len(entries) != expected_entries:
        failures.append(
            f"provenance entry count mismatch: expected={expected_entries} "
            f"actual={len(entries)}"
        )

    seen: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            failures.append(f"entry {index} is not an object")
            continue
        relative = entry.get("path")
        if not isinstance(relative, str):
            failures.append(f"entry {index} has no string path")
            continue
        normalized = PurePosixPath(relative.replace("\\", "/"))
        if normalized.is_absolute() or ".." in normalized.parts:
            failures.append(f"unsafe provenance path: {relative}")
            continue
        normalized_text = normalized.as_posix()
        if normalized_text in seen:
            failures.append(f"duplicate provenance path: {normalized_text}")
            continue
        seen.add(normalized_text)
        member = package_root.joinpath(*normalized.parts)
        try:
            resolved = member.resolve(strict=True)
            resolved.relative_to(package_root)
        except (FileNotFoundError, ValueError) as exc:
            failures.append(f"missing/escaping member {normalized_text}: {exc}")
            continue
        if not resolved.is_file():
            failures.append(f"provenance member is not a file: {normalized_text}")
            continue
        expected_bytes = entry.get("bytes")
        actual_bytes = resolved.stat().st_size
        if not isinstance(expected_bytes, int) or actual_bytes != expected_bytes:
            failures.append(
                f"size mismatch {normalized_text}: expected={expected_bytes} "
                f"actual={actual_bytes}"
            )
            continue
        expected_sha = entry.get("sha256")
        actual_sha = sha256_file(resolved)
        if not isinstance(expected_sha, str) or actual_sha != expected_sha.lower():
            failures.append(
                f"sha256 mismatch {normalized_text}: expected={expected_sha} "
                f"actual={actual_sha}"
            )
            continue
        verified += 1
        verified_bytes += actual_bytes

    return {
        "status": "passed" if not failures else "failed",
        "provenance": str(provenance_path),
        "provenance_sha256": sha256_file(provenance_path),
        "expected_entries": expected_entries,
        "declared_entries": len(entries),
        "verified_entries": verified,
        "verified_bytes": verified_bytes,
        "failures": failures,
    }


def validate_declared_contract(package_root: Path) -> dict[str, Any]:
    """Cross-check provenance, TFLite metadata, and qualification evidence."""

    package_root = package_root.resolve()
    failures: list[str] = []
    try:
        provenance = _load_json(package_root / "provenance.json")
        contract = _load_json(package_root / "models/tflite_contract.json")
        qualification = _load_json(
            package_root / "metrics/qualification_summary.json"
        )
    except Exception as exc:
        return {
            "status": "failed",
            "failures": [f"contract evidence unreadable: {type(exc).__name__}: {exc}"],
        }

    _expect(failures, "model_family", provenance.get("model_family"), "YOLOX-Tiny")
    _expect(
        failures,
        "source_split_manifest_sha256",
        provenance.get("source_split_manifest_sha256"),
        EXPECTED_SPLIT_SHA256,
    )
    input_contract = provenance.get("input_contract", {})
    output_contract = provenance.get("output_contract", {})
    _expect(failures, "provenance input shape", input_contract.get("shape_tflite"), EXPECTED_INPUT_SHAPE)
    _expect(failures, "provenance input color", input_contract.get("color"), "BGR")
    _expect(failures, "provenance input range", input_contract.get("range"), [0, 255])
    _expect(failures, "provenance output shape", output_contract.get("shape"), EXPECTED_OUTPUT_SHAPE)
    _expect(failures, "provenance output decoded", output_contract.get("decoded"), False)
    _expect(failures, "provenance embedded NMS", output_contract.get("embedded_nms"), False)

    declared_int8 = contract.get("models", {}).get("int8", {})
    declared_input = declared_int8.get("input", {})
    declared_output = declared_int8.get("output", {})
    _expect(failures, "TFLite bytes", declared_int8.get("bytes"), 5340416)
    _expect(failures, "TFLite sha256", declared_int8.get("sha256"), EXPECTED_MODEL_SHA256)
    _expect(failures, "TFLite input shape", declared_input.get("shape"), EXPECTED_INPUT_SHAPE)
    _expect(failures, "TFLite input dtype", declared_input.get("dtype"), "<class 'numpy.int8'>")
    _expect_sequence_close(
        failures,
        "TFLite input quantization",
        declared_input.get("quantization"),
        EXPECTED_INPUT_QUANTIZATION,
    )
    _expect(failures, "TFLite output shape", declared_output.get("shape"), EXPECTED_OUTPUT_SHAPE)
    _expect(failures, "TFLite output dtype", declared_output.get("dtype"), "<class 'numpy.int8'>")
    _expect_sequence_close(
        failures,
        "TFLite output quantization",
        declared_output.get("quantization"),
        EXPECTED_OUTPUT_QUANTIZATION,
    )
    ruhmi_import = contract.get("ruhmi_import", {})
    _expect(failures, "RUHMI target", ruhmi_import.get("target"), "npu")
    _expect(failures, "RUHMI quantize", ruhmi_import.get("quantize"), False)

    qualified_int8 = qualification.get("validation_metrics", {}).get(
        "tflite_int8", {}
    )
    qualified_input = qualified_int8.get("input_contract", {})
    thresholds = qualified_int8.get("thresholds", {})
    _expect(failures, "qualified input shape", qualified_input.get("tile_shape"), EXPECTED_INPUT_SHAPE)
    _expect(failures, "qualified color", qualified_input.get("color"), "BGR")
    _expect(failures, "qualified range", qualified_input.get("range"), [0, 255])
    _expect(failures, "tile origins x", qualified_input.get("tile_origins_x"), EXPECTED_TILE_X)
    _expect(failures, "tile origins y", qualified_input.get("tile_origins_y"), EXPECTED_TILE_Y)
    _expect(failures, "tiles per frame", qualified_input.get("tiles_per_frame"), 12)
    _expect_close(failures, "confidence", thresholds.get("confidence"), EXPECTED_CONFIDENCE)
    _expect(failures, "tile local NMS", thresholds.get("tile_local_nms"), False)
    _expect_close(failures, "global NMS IoU", thresholds.get("global_nms_iou"), EXPECTED_NMS_IOU)
    _expect_close(failures, "match IoU", thresholds.get("match_iou"), EXPECTED_MATCH_IOU)

    return {
        "status": "passed" if not failures else "failed",
        "model_relative_path": EXPECTED_MODEL_RELATIVE_PATH.as_posix(),
        "model_sha256": EXPECTED_MODEL_SHA256,
        "input": {
            "shape": EXPECTED_INPUT_SHAPE,
            "dtype": "int8",
            "quantization": list(EXPECTED_INPUT_QUANTIZATION),
            "color": "BGR",
            "semantic_range": [0, 255],
            "normalization": "none",
        },
        "output": {
            "shape": EXPECTED_OUTPUT_SHAPE,
            "dtype": "int8",
            "quantization": list(EXPECTED_OUTPUT_QUANTIZATION),
            "decoded": False,
        },
        "tiling": {
            "reference_frame": [640, 480],
            "tile_size": [224, 224],
            "origins_x": EXPECTED_TILE_X,
            "origins_y": EXPECTED_TILE_Y,
            "tiles_per_frame": 12,
        },
        "thresholds": {
            "confidence": EXPECTED_CONFIDENCE,
            "global_nms_iou": EXPECTED_NMS_IOU,
            "match_iou": EXPECTED_MATCH_IOU,
        },
        "ruhmi_import": {"target": "npu", "quantize": False},
        "failures": failures,
    }


def validate_runtime_tflite(
    model_path: Path,
    *,
    interpreter_factory: Callable[..., Any] | None = None,
) -> dict[str, Any]:
    """Read the ABI from the actual flatbuffer, not only the handoff JSON."""

    failures: list[str] = []
    try:
        if interpreter_factory is None:
            interpreter_factory = _tensorflow_interpreter_factory()
        interpreter = interpreter_factory(model_path=str(model_path.resolve()))
        interpreter.allocate_tensors()
        inputs = interpreter.get_input_details()
        outputs = interpreter.get_output_details()
    except Exception as exc:
        return {
            "status": "failed",
            "failures": [f"TFLite interpreter unavailable/invalid: {type(exc).__name__}: {exc}"],
        }

    if len(inputs) != 1:
        failures.append(f"expected one TFLite input, found {len(inputs)}")
    if len(outputs) != 1:
        failures.append(f"expected one TFLite output, found {len(outputs)}")
    input_detail = inputs[0] if len(inputs) == 1 else {}
    output_detail = outputs[0] if len(outputs) == 1 else {}
    _expect(failures, "runtime input shape", _shape_list(input_detail), EXPECTED_INPUT_SHAPE)
    _expect(failures, "runtime output shape", _shape_list(output_detail), EXPECTED_OUTPUT_SHAPE)
    _expect(failures, "runtime input dtype", _dtype_name(input_detail), "int8")
    _expect(failures, "runtime output dtype", _dtype_name(output_detail), "int8")
    _expect_sequence_close(
        failures,
        "runtime input quantization",
        input_detail.get("quantization"),
        EXPECTED_INPUT_QUANTIZATION,
    )
    _expect_sequence_close(
        failures,
        "runtime output quantization",
        output_detail.get("quantization"),
        EXPECTED_OUTPUT_QUANTIZATION,
    )
    return {
        "status": "passed" if not failures else "failed",
        "model": str(model_path.resolve()),
        "model_bytes": model_path.stat().st_size if model_path.is_file() else None,
        "model_sha256": sha256_file(model_path) if model_path.is_file() else None,
        "interpreter": f"{type(interpreter).__module__}.{type(interpreter).__name__}",
        "input": _detail_summary(input_detail),
        "output": _detail_summary(output_detail),
        "failures": failures,
    }


def validate_environment(ruhmi_root: Path) -> dict[str, Any]:
    """Prove the exact RUHMI checkout, MERA, Vela, and Python environment."""

    ruhmi_root = ruhmi_root.resolve()
    repo = ruhmi_root / "ruhmi-framework-mcu"
    scripts = ruhmi_root / "venv-2.6.0" / "Scripts"
    python_exe = scripts / "python.exe"
    vela_exe = scripts / "vela.exe"
    failures: list[str] = []
    observations: dict[str, Any] = {
        "ruhmi_root": str(ruhmi_root),
        "repo": str(repo),
        "scripts": str(scripts),
        "python": str(python_exe),
        "vela": str(vela_exe),
    }
    for label, path in (("RUHMI repo", repo), ("pinned Python", python_exe), ("pinned Vela", vela_exe)):
        if not path.exists():
            failures.append(f"{label} missing: {path}")
        if not _is_ascii_path(path):
            failures.append(f"{label} path is not ASCII: {path}")

    if not failures:
        probes = {
            "ruhmi_head": (["git", "-C", str(repo), "rev-parse", "HEAD"], EXPECTED_RUHMI_HEAD),
            "ruhmi_tag": (["git", "-C", str(repo), "describe", "--tags", "--exact-match"], EXPECTED_RUHMI_TAG),
            "mera_version": ([str(python_exe), "-c", "import mera; print(mera.__version__)"], EXPECTED_MERA_VERSION),
            "vela_version": ([str(vela_exe), "--version"], EXPECTED_VELA_VERSION),
        }
        for label, (command, expected) in probes.items():
            try:
                completed = subprocess.run(
                    command,
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    encoding="utf-8",
                    errors="replace",
                )
                observed = completed.stdout.strip()
                observations[label] = observed
                if observed != expected:
                    failures.append(
                        f"{label} mismatch: expected={expected} actual={observed}"
                    )
            except Exception as exc:
                failures.append(f"{label} probe failed: {type(exc).__name__}: {exc}")

    observations.update(
        {
            "host_python": sys.version,
            "host_python_executable": sys.executable,
            "host_platform": platform.platform(),
            "numpy": np.__version__,
            "expected": {
                "ruhmi_head": EXPECTED_RUHMI_HEAD,
                "ruhmi_tag": EXPECTED_RUHMI_TAG,
                "mera": EXPECTED_MERA_VERSION,
                "vela": EXPECTED_VELA_VERSION,
            },
        }
    )
    observations["status"] = "passed" if not failures else "failed"
    observations["failures"] = failures
    return observations


def prepare_ascii_staging(
    source_model: Path,
    staging_root: Path,
    *,
    expected_sha256: str = EXPECTED_MODEL_SHA256,
) -> dict[str, Any]:
    """Copy the immutable model to a hash-keyed ASCII path without overwrite."""

    source_model = source_model.resolve()
    staging_root = staging_root.resolve()
    destination = staging_root / expected_sha256 / "model.tflite"
    failures: list[str] = []
    if not source_model.is_file():
        failures.append(f"source model missing: {source_model}")
    if not _is_ascii_path(staging_root) or not _is_ascii_path(destination):
        failures.append(f"staging path is not ASCII: {destination}")
    source_sha = sha256_file(source_model) if source_model.is_file() else None
    if source_sha != expected_sha256:
        failures.append(
            f"source model sha256 mismatch: expected={expected_sha256} actual={source_sha}"
        )

    action = "blocked"
    staged_sha: str | None = None
    if not failures:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            if not destination.is_file():
                failures.append(f"staging destination is not a file: {destination}")
            else:
                staged_sha = sha256_file(destination)
                if staged_sha != expected_sha256:
                    failures.append(
                        f"existing staging hash mismatch: expected={expected_sha256} "
                        f"actual={staged_sha}"
                    )
                else:
                    action = "reused"
        else:
            shutil.copy2(source_model, destination)
            staged_sha = sha256_file(destination)
            if staged_sha != expected_sha256:
                failures.append(
                    f"copied staging hash mismatch: expected={expected_sha256} "
                    f"actual={staged_sha}"
                )
            else:
                action = "created"
    return {
        "status": "passed" if not failures else "failed",
        "action": action,
        "source": str(source_model),
        "source_sha256": source_sha,
        "destination": str(destination),
        "destination_ascii": _is_ascii_path(destination),
        "staged_sha256": staged_sha,
        "failures": failures,
    }


def run_preflight(
    package_root: Path,
    ruhmi_root: Path,
    staging_root: Path,
) -> dict[str, Any]:
    package_root = package_root.resolve()
    model_path = package_root / EXPECTED_MODEL_RELATIVE_PATH
    package = verify_provenance(package_root)
    declared = validate_declared_contract(package_root)
    runtime = validate_runtime_tflite(model_path)
    environment = validate_environment(ruhmi_root)
    prerequisites = (package, declared, runtime, environment)
    if all(item.get("status") == "passed" for item in prerequisites):
        staging = prepare_ascii_staging(model_path, staging_root)
    else:
        staging = {
            "status": "blocked",
            "action": "not-attempted",
            "failures": ["package/model-contract/environment prerequisite failed"],
        }
    passed = all(
        item.get("status") == "passed"
        for item in (package, declared, runtime, environment, staging)
    )
    return {
        "schema_version": 1,
        "status": "preflight_passed" if passed else "preflight_failed",
        "purpose": "yolox-tiny-tile224-ruhmi-intake",
        "compile_started": False,
        "firmware_integration_allowed": False,
        "package": package,
        "model_contract": {"declared": declared, "runtime": runtime},
        "environment": environment,
        "staging": staging,
        "next_gate": (
            "pinned_ruhmi_target_npu_quantize_false"
            if passed
            else "fix_preflight_failures_without_changing_model"
        ),
    }


def inspect_compiler_output(
    compiler_output_dir: Path,
    firmware_memory_regions: Path,
    *,
    compile_log: Path | None = None,
    firmware_bsp_config: Path | None = None,
) -> dict[str, Any]:
    """Audit generated MERA/Vela artifacts without copying them into firmware."""

    compiler_output_dir = compiler_output_dir.resolve()
    firmware_memory_regions = firmware_memory_regions.resolve()
    failures: list[str] = []
    try:
        subgraphs_path = _find_unique_file(
            compiler_output_dir, "model_subgraphs.json"
        )
        io_path = _find_unique_file(compiler_output_dir, "io_desc.json")
        deploy_cfg_path = _find_unique_file(compiler_output_dir, "deploy_cfg.json")
        summary_path = _find_unique_file(
            compiler_output_dir, "model_summary_RA8P1.csv"
        )
        model_data_path = _find_unique_pattern(
            compiler_output_dir, "sub_*_model_data.c"
        )
        subgraphs = _load_json(subgraphs_path)
        io_desc = _load_json(io_path)
        deploy_cfg = _load_json(deploy_cfg_path)
        summary = _load_single_csv_row(summary_path)
        flash_length = _parse_linker_length(
            firmware_memory_regions, "FLASH_LENGTH"
        )
    except Exception as exc:
        return {
            "schema_version": 1,
            "status": "compiler_output_rejected",
            "firmware_integration_allowed": False,
            "failures": [
                f"compiler evidence unreadable: {type(exc).__name__}: {exc}"
            ],
        }

    regions: list[dict[str, Any]] = []
    for index, subgraph in enumerate(subgraphs.get("subgraphs", [])):
        target = str(subgraph.get("target_name", "UNKNOWN"))
        operators = [
            str(node.get("op_name"))
            for node in subgraph.get("nodes", [])
            if node.get("op_name")
        ]
        executable = [
            operator
            for operator in operators
            if operator not in _NON_EXECUTING_REGION_OPERATORS
        ]
        regions.append(
            {
                "index": index,
                "name": str(subgraph.get("subgraph_name", f"sub_{index}")),
                "target": target,
                "operators": sorted(set(operators)),
                "executable_operators": executable,
            }
        )

    cpu_regions = [region for region in regions if region["target"] == "C_CODEGEN"]
    npu_regions = [
        region for region in regions if "ARM_ETHOS_U55" in region["target"]
    ]
    cpu_focus_calls = sum(
        region["executable_operators"].count(EXPECTED_CPU_FOCUS_OPERATOR)
        for region in cpu_regions
    )
    cpu_only_focus = all(
        region["executable_operators"] == [EXPECTED_CPU_FOCUS_OPERATOR]
        for region in cpu_regions
    )
    npu_operator_set = {
        operator
        for region in npu_regions
        for operator in region["executable_operators"]
    }


    expected_io = {
        "inputs": [["serving_default_images_0", ["1", "224", "224", "3"], "Int8"]],
        "outputs": [["PartitionedCall_0_70473", ["1", "1029", "6"], "Int8"]],
    }
    io_matches = io_desc == expected_io
    if not io_matches:
        failures.append(f"generated IO mismatch: expected={expected_io!r} actual={io_desc!r}")

    try:
        on_chip_flash_kib = float(summary["on_chip_flash_memory_used"])
        off_chip_flash_kib = float(summary["off_chip_flash_memory_used"])
        vela_sram_kib = float(summary["sram_memory_used"])
        estimated_seconds = float(summary["inference_time"])
    except (KeyError, TypeError, ValueError) as exc:
        failures.append(f"Vela memory summary invalid: {type(exc).__name__}: {exc}")
        on_chip_flash_kib = math.inf
        off_chip_flash_kib = math.inf
        vela_sram_kib = math.inf
        estimated_seconds = math.inf

    on_chip_flash_bytes = round(on_chip_flash_kib * 1024.0)
    ccodegen = (
        deploy_cfg.get("backend_options", {}).get("ccodegen", {})
    )
    ospi_enabled_in_compile = ccodegen.get("enable_ospi") is True
    model_data_text = model_data_path.read_text(encoding="utf-8", errors="replace")
    model_data_size_match = re.search(
        r"sub_\d+_model_data_size\s*=\s*(\d+)", model_data_text
    )
    model_data_bytes = (
        int(model_data_size_match.group(1))
        if model_data_size_match is not None
        else model_data_path.stat().st_size
    )
    model_data_uses_ospi_load = (
        'section(".sdram_from_ospi0_cs1")' in model_data_text
    )
    invoke_sources = sorted(compiler_output_dir.rglob("sub_*_invoke.c"))
    npu_arenas_use_sdram = bool(invoke_sources) and all(
        'section(".sdram")' in path.read_text(encoding="utf-8", errors="replace")
        for path in invoke_sources
    )
    if ospi_enabled_in_compile:
        storage_mode = "external_ospi_to_sdram"
        storage_contract_consistent = (
            model_data_uses_ospi_load and npu_arenas_use_sdram
        )
        selected_storage_fits = (
            model_data_bytes <= EXPECTED_BOARD_OSPI_BYTES
            and model_data_bytes + round(vela_sram_kib * 1024.0)
            <= EXPECTED_BOARD_SDRAM_BYTES
        )
    else:
        storage_mode = "internal_flash"
        storage_contract_consistent = (
            not model_data_uses_ospi_load
            and summary.get("memory_mode") == "Sram_Only"
            and off_chip_flash_kib == 0.0
            and summary.get("weights_storage_area") == "On-chip Flash"
        )
        selected_storage_fits = on_chip_flash_bytes <= flash_length

    ospi_startup_enabled: bool | None = None
    bsp_config_record: dict[str, Any] | None = None
    if firmware_bsp_config is not None:
        resolved_bsp_config = firmware_bsp_config.resolve()
        if resolved_bsp_config.is_file():
            bsp_config_record = _file_record(resolved_bsp_config)
            ospi_startup_enabled = _parse_c_macro_int(
                resolved_bsp_config, "BSP_CFG_OSPI_B_STARTUP_ENABLED"
            ) == 1
        else:
            failures.append(f"firmware BSP config missing: {resolved_bsp_config}")
    firmware_storage_ready = (
        not ospi_enabled_in_compile or ospi_startup_enabled is True
    )

    generated_sources = sorted(compiler_output_dir.rglob("model.c"))
    saved_models = sorted(compiler_output_dir.rglob("model.mera"))
    compile_log_record: dict[str, Any] | None = None
    if compile_log is not None:
        resolved_log = compile_log.resolve()
        if resolved_log.is_file():
            compile_log_record = _file_record(resolved_log)
        else:
            failures.append(f"compile log missing: {resolved_log}")

    generated_memory = _inspect_generated_mutable_memory(compiler_output_dir)
    operator_acceptance = {
        "cpu_region_count_is_four": len(cpu_regions) == EXPECTED_CPU_FOCUS_REGIONS,
        "cpu_regions_are_focus_only": cpu_only_focus,
        "cpu_focus_call_count_is_four": cpu_focus_calls == EXPECTED_CPU_FOCUS_REGIONS,
        "single_npu_region": len(npu_regions) == EXPECTED_NPU_REGION_COUNT,
        "npu_contains_convolution_backbone": "TFLiteQConv2dBias" in npu_operator_set,
        "npu_contains_both_upsampling_stages": sum(
            region["executable_operators"].count("Upsampling")
            for region in npu_regions
        )
        == 2,
    }
    artifact_acceptance = {
        "generated_model_c_exists": len(generated_sources) == 1,
        "generated_io_matches_frozen_abi": io_matches,
        "generated_storage_contract_is_consistent": storage_contract_consistent,
    }
    reload_acceptance = {
        "saved_model_exists": len(saved_models) == 1,
        "full_console_log_exists": compile_log_record is not None,
    }
    memory_acceptance = {
        "generated_constants_fit_selected_storage": selected_storage_fits,
        "firmware_storage_startup_matches_compile": firmware_storage_ready,
    }
    compiled = all(operator_acceptance.values()) and all(artifact_acceptance.values())
    host_gate_qualified = (
        compiled
        and all(reload_acceptance.values())
        and all(memory_acceptance.values())
        and not failures
    )

    if not compiled:
        status = "compiler_output_rejected"
        next_gate = "fix_compile_or_allocation_without_touching_firmware"
    elif not all(memory_acceptance.values()):
        status = "host_gate_blocked"
        next_gate = (
            "qualify_external_constant_storage_and_mutable_buffer_placement"
            if storage_mode == "internal_flash"
            else "enable_and_hardware_qualify_ospi_to_sdram_startup"
        )
    elif not all(reload_acceptance.values()):
        status = "host_gate_blocked"
        next_gate = "establish_fresh_process_reload_and_complete_log_evidence"
    else:
        status = "host_gate_qualified" if host_gate_qualified else "host_gate_blocked"
        next_gate = "firmware_integration" if host_gate_qualified else "fix_evidence_gaps"

    return {
        "schema_version": 1,
        "status": status,
        "firmware_integration_allowed": host_gate_qualified,
        "compiler_output_dir": str(compiler_output_dir),
        "evidence": {
            "model_subgraphs": _file_record(subgraphs_path),
            "io_desc": _file_record(io_path),
            "deploy_cfg": _file_record(deploy_cfg_path),
            "vela_summary": _file_record(summary_path),
            "model_data_source": _file_record(model_data_path),
            "firmware_memory_regions": _file_record(firmware_memory_regions),
            "firmware_bsp_config": bsp_config_record,
            "compile_log": compile_log_record,
            "generated_model_sources": [
                _file_record(path) for path in generated_sources
            ],
            "saved_models": [_file_record(path) for path in saved_models],
        },
        "allocation": {
            "regions": regions,
            "cpu_region_count": len(cpu_regions),
            "cpu_focus_call_count": cpu_focus_calls,
            "npu_region_count": len(npu_regions),
            "operator_acceptance": operator_acceptance,
        },
        "generated_io": io_desc,
        "memory": {
            "vela": {
                "memory_mode": summary.get("memory_mode"),
                "weights_storage_area": summary.get("weights_storage_area"),
                "feature_map_storage_area": summary.get("feature_map_storage_area"),
                "on_chip_flash_bytes": on_chip_flash_bytes,
                "off_chip_flash_bytes": round(off_chip_flash_kib * 1024.0),
                "sram_bytes": round(vela_sram_kib * 1024.0),
                "estimated_npu_seconds": estimated_seconds,
                "estimated_npu_time_is_not_board_latency": True,
            },
            "generated_storage": {
                "mode": storage_mode,
                "enable_ospi": ospi_enabled_in_compile,
                "model_data_bytes": model_data_bytes,
                "model_data_uses_sdram_from_ospi0_cs1": model_data_uses_ospi_load,
                "npu_arenas_use_sdram": npu_arenas_use_sdram,
                "board_ospi_bytes": EXPECTED_BOARD_OSPI_BYTES,
                "board_sdram_bytes": EXPECTED_BOARD_SDRAM_BYTES,
            },
            "firmware": {
                "internal_flash_bytes": flash_length,
                "ospi_startup_enabled": ospi_startup_enabled,
                "generated_constants_excess_bytes": max(
                    on_chip_flash_bytes - flash_length, 0
                ) if storage_mode == "internal_flash" else 0,
            },
            "generated_mutable_buffers": generated_memory,
            "acceptance": memory_acceptance,
        },
        "artifact_acceptance": artifact_acceptance,
        "reload_acceptance": reload_acceptance,
        "next_gate": next_gate,
        "failures": failures,
    }


def compare_int8_raw_output(
    actual: np.ndarray,
    expected: np.ndarray,
    *,
    maximum_absolute_error: int = 1,
) -> dict[str, Any]:
    """Compare a freshly reloaded raw head with the frozen INT8 golden."""

    failures: list[str] = []
    if actual.shape != tuple(EXPECTED_OUTPUT_SHAPE):
        failures.append(
            f"actual output shape mismatch: expected={EXPECTED_OUTPUT_SHAPE} "
            f"actual={list(actual.shape)}"
        )
    if expected.shape != tuple(EXPECTED_OUTPUT_SHAPE):
        failures.append(
            f"golden output shape mismatch: expected={EXPECTED_OUTPUT_SHAPE} "
            f"actual={list(expected.shape)}"
        )
    if actual.dtype != np.int8:
        failures.append(
            f"actual output dtype mismatch: expected=int8 actual={actual.dtype}"
        )
    if expected.dtype != np.int8:
        failures.append(
            f"golden output dtype mismatch: expected=int8 actual={expected.dtype}"
        )

    same_shape = actual.shape == expected.shape
    if same_shape:
        difference = np.abs(actual.astype(np.int16) - expected.astype(np.int16))
        max_error = int(difference.max(initial=0))
        changed_values = int(np.count_nonzero(difference))
    else:
        max_error = None
        changed_values = None
    nonconstant = bool(actual.size and int(actual.max()) != int(actual.min()))
    within_tolerance = max_error is not None and max_error <= maximum_absolute_error
    if not nonconstant:
        failures.append("actual output is constant")
    if not within_tolerance:
        failures.append(
            f"raw output error exceeds tolerance: allowed={maximum_absolute_error} "
            f"actual={max_error}"
        )
    return {
        "status": "passed" if not failures else "failed",
        "maximum_absolute_error": max_error,
        "allowed_absolute_error": maximum_absolute_error,
        "changed_values": changed_values,
        "element_count": int(actual.size),
        "nonconstant": nonconstant,
        "failures": failures,
    }


def write_report(report: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        loaded = json.load(stream)
    if not isinstance(loaded, dict):
        raise ValueError(f"expected JSON object: {path}")
    return loaded


def _load_single_csv_row(path: Path) -> dict[str, str]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise ValueError(f"expected exactly one CSV row: {path}")
    return rows[0]


def _find_unique_file(root: Path, name: str) -> Path:
    matches = sorted(path for path in root.rglob(name) if path.is_file())
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {name}, found {len(matches)} under {root}")
    return matches[0]


def _find_unique_pattern(root: Path, pattern: str) -> Path:
    matches = sorted(path for path in root.rglob(pattern) if path.is_file())
    if len(matches) != 1:
        raise ValueError(
            f"expected exactly one {pattern}, found {len(matches)} under {root}"
        )
    return matches[0]


def _parse_linker_length(path: Path, symbol: str) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"^\s*{re.escape(symbol)}\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing linker symbol {symbol}: {path}")
    return int(match.group(1), 0)


def _parse_c_macro_int(path: Path, macro: str) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(
        rf"^\s*#\s*define\s+{re.escape(macro)}\s+\(?\s*(\d+)\s*\)?",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise ValueError(f"missing C macro {macro}: {path}")
    return int(match.group(1))


def _file_record(path: Path) -> dict[str, Any]:
    return {
        "path": str(path.resolve()),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def _inspect_generated_mutable_memory(root: Path) -> dict[str, Any]:
    model_source = _find_unique_file(root, "model.c")
    model_text = model_source.read_text(encoding="utf-8", errors="replace")
    cpu_buffers = [
        {"name": name, "bytes": int(size)}
        for name, size in re.findall(
            r"^\s*(?:u?int8_t)\s+(buf_[A-Za-z0-9_]+)\[(\d+)\]\s*;",
            model_text,
            re.MULTILINE,
        )
    ]
    invoke_sources = sorted(root.rglob("sub_*_invoke.c"))
    npu_arenas: list[dict[str, Any]] = []
    for source in invoke_sources:
        text = source.read_text(encoding="utf-8", errors="replace")
        npu_arenas.extend(
            {"name": name, "bytes": int(size)}
            for name, size in re.findall(
                r"^\s*(?:__attribute__\(\(aligned\(\d+\)\)\)\s+)?uint8_t\s+"
                r"(sub_[A-Za-z0-9_]+_arena)\[(\d+)\]\s*;",
                text,
                re.MULTILINE,
            )
        )
    cpu_arenas: list[dict[str, Any]] = []
    for header in sorted(root.rglob("compute_sub_*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"kBufferSize_(sub_\d+)\s*=\s*(\d+)", text)
        if match is not None:
            cpu_arenas.append(
                {"name": f"compute_arena_{match.group(1)}", "bytes": int(match.group(2))}
            )
    lower_bound = sum(
        record["bytes"] for record in cpu_buffers + npu_arenas + cpu_arenas
    )
    return {
        "cpu_io_buffers": cpu_buffers,
        "npu_arenas": npu_arenas,
        "cpu_arenas": cpu_arenas,
        "static_mutable_bytes_lower_bound": lower_bound,
        "placement_note": (
            "Generated arrays have no project-specific SDRAM placement yet; "
            "the firmware linker must prove their final regions."
        ),
    }


def _tensorflow_interpreter_factory() -> Callable[..., Any]:
    import tensorflow as tf

    return tf.lite.Interpreter


def _shape_list(detail: dict[str, Any]) -> list[int] | None:
    shape = detail.get("shape")
    return [int(value) for value in shape] if shape is not None else None


def _dtype_name(detail: dict[str, Any]) -> str | None:
    dtype = detail.get("dtype")
    return np.dtype(dtype).name if dtype is not None else None


def _detail_summary(detail: dict[str, Any]) -> dict[str, Any]:
    quantization = detail.get("quantization")
    return {
        "name": detail.get("name"),
        "shape": _shape_list(detail),
        "dtype": _dtype_name(detail),
        "quantization": list(quantization) if quantization is not None else None,
    }


def _is_ascii_path(path: Path) -> bool:
    return str(path).isascii()


def _expect(failures: list[str], label: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        failures.append(f"{label} mismatch: expected={expected!r} actual={actual!r}")


def _expect_close(
    failures: list[str], label: str, actual: Any, expected: float
) -> None:
    try:
        matches = math.isclose(float(actual), expected, rel_tol=0.0, abs_tol=1e-12)
    except (TypeError, ValueError):
        matches = False
    if not matches:
        failures.append(f"{label} mismatch: expected={expected!r} actual={actual!r}")


def _expect_sequence_close(
    failures: list[str], label: str, actual: Any, expected: tuple[float, int]
) -> None:
    if not isinstance(actual, (list, tuple)) or len(actual) != 2:
        failures.append(f"{label} mismatch: expected={list(expected)!r} actual={actual!r}")
        return
    _expect_close(failures, f"{label} scale", actual[0], expected[0])
    try:
        actual_zero_point = int(actual[1])
    except (TypeError, ValueError):
        actual_zero_point = actual[1]
    _expect(failures, f"{label} zero_point", actual_zero_point, expected[1])
