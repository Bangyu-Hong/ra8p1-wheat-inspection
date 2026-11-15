"""Build and qualify fixed-shape ConvTranspose operator smoke models."""

from __future__ import annotations

import argparse
import copy
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import platform
import sys
import time

import numpy as np


SOURCE_INPUT_NAME = "images"
EXPECTED_SOURCE_INPUT_SHAPE = (1, 3, 320, 320)
DEFAULT_CALIBRATION_COUNT = 35
EXPECTED_SOURCE_MODEL_SHA256 = (
    "a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2"
)
LEARNED_OPERATOR_TOKENS = (
    "ConvTranspose",
    "TransposeConv",
    "Conv2DTranspose",
    "TransposedConv",
)


@dataclass(frozen=True)
class ConvTransposeSmokeSpec:
    key: str
    source_tensor: str
    channels: int
    input_size: int

    @property
    def input_shape(self) -> tuple[int, int, int, int]:
        return (1, self.channels, self.input_size, self.input_size)

    @property
    def output_shape(self) -> tuple[int, int, int, int]:
        output_size = self.input_size * 2
        return (1, self.channels, output_size, output_size)


SMOKE_SPECS = (
    ConvTransposeSmokeSpec(
        key="c256_10_to_20",
        source_tensor="/model.9/cv2/act/Mul_output_0",
        channels=256,
        input_size=10,
    ),
    ConvTransposeSmokeSpec(
        key="c128_20_to_40",
        source_tensor="/model.12/cv2/act/Mul_output_0",
        channels=128,
        input_size=20,
    ),
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tensor_sha256(values: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(values).tobytes()).hexdigest()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _tensor_record(values: np.ndarray) -> dict[str, object]:
    return {
        "shape": list(values.shape),
        "dtype": str(values.dtype),
        "minimum": float(np.min(values)),
        "maximum": float(np.max(values)),
        "dynamic_range": float(np.ptp(values)),
        "finite": bool(np.all(np.isfinite(values))),
        "tensor_sha256": tensor_sha256(values),
    }


def get_smoke_spec(key: str) -> ConvTransposeSmokeSpec:
    for spec in SMOKE_SPECS:
        if spec.key == key:
            return spec
    raise ValueError(f"unknown ConvTranspose smoke shape: {key}")


def build_smoke_model(spec: ConvTransposeSmokeSpec, output_path: Path) -> dict[str, object]:
    try:
        import onnx
        from onnx import TensorProto, helper, numpy_helper
    except ImportError as exc:
        raise RuntimeError("onnx is required to build smoke models") from exc

    weights = np.zeros(
        (spec.channels, spec.channels, 2, 2),
        dtype=np.float32,
    )
    channel_indices = np.arange(spec.channels)
    weights[channel_indices, channel_indices, :, :] = np.float32(1.0)
    bias = np.zeros((spec.channels,), dtype=np.float32)

    node = helper.make_node(
        "ConvTranspose",
        inputs=["input", "weight", "bias"],
        outputs=["output"],
        name=f"{spec.key}_convtranspose",
        dilations=[1, 1],
        group=1,
        kernel_shape=[2, 2],
        output_padding=[0, 0],
        pads=[0, 0, 0, 0],
        strides=[2, 2],
    )
    graph = helper.make_graph(
        [node],
        f"ra8p1_{spec.key}_convtranspose_smoke",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT, spec.input_shape)],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, spec.output_shape)],
        [
            numpy_helper.from_array(weights, name="weight"),
            numpy_helper.from_array(bias, name="bias"),
        ],
    )
    model = helper.make_model(
        graph,
        producer_name="ra8p1-wheat-convtranspose-smoke",
        opset_imports=[helper.make_opsetid("", 13)],
    )
    model.ir_version = 7
    onnx.checker.check_model(model, full_check=True)
    inferred = onnx.shape_inference.infer_shapes(model, strict_mode=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(inferred, output_path)

    return {
        "path": output_path.as_posix(),
        "bytes": output_path.stat().st_size,
        "sha256": sha256_file(output_path),
        "operator": "ConvTranspose",
        "opset": 13,
        "input_shape": list(spec.input_shape),
        "output_shape": list(spec.output_shape),
        "kernel_shape": [2, 2],
        "strides": [2, 2],
        "group": 1,
        "weight_shape": list(weights.shape),
        "weight_nonzero_count": int(np.count_nonzero(weights)),
        "weight_sha256": tensor_sha256(weights),
        "bias_sha256": tensor_sha256(bias),
    }


def _load_calibration_paths(directory: Path, expected_count: int) -> list[Path]:
    if expected_count <= 0:
        raise ValueError("calibration tensor count must be positive")
    paths = sorted(directory.glob("*.npy"))
    if len(paths) != expected_count:
        raise ValueError(
            f"expected {expected_count} calibration tensors in {directory}, "
            f"found {len(paths)}"
        )
    return paths


def _build_source_feature_model(source_path: Path, output_path: Path) -> None:
    try:
        import onnx
        from onnx import TensorProto, helper
    except ImportError as exc:
        raise RuntimeError("onnx is required to expose source features") from exc

    source = onnx.load(source_path)
    onnx.checker.check_model(source, full_check=True)
    inferred = onnx.shape_inference.infer_shapes(source, strict_mode=True)
    known_shapes: dict[str, tuple[int, ...]] = {}
    for value in (
        list(inferred.graph.input)
        + list(inferred.graph.value_info)
        + list(inferred.graph.output)
    ):
        tensor_type = value.type.tensor_type
        if not tensor_type.HasField("shape"):
            continue
        dimensions = tuple(dimension.dim_value for dimension in tensor_type.shape.dim)
        if dimensions and all(dimensions):
            known_shapes[value.name] = dimensions

    feature_model = copy.deepcopy(inferred)
    del feature_model.graph.output[:]
    for spec in SMOKE_SPECS:
        actual_shape = known_shapes.get(spec.source_tensor)
        if actual_shape != spec.input_shape:
            raise ValueError(
                f"source feature shape changed for {spec.source_tensor}: "
                f"expected={spec.input_shape} actual={actual_shape}"
            )
        feature_model.graph.output.append(
            helper.make_tensor_value_info(
                spec.source_tensor,
                TensorProto.FLOAT,
                spec.input_shape,
            )
        )
    onnx.checker.check_model(feature_model, full_check=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(feature_model, output_path)


def extract_feature_calibration(
    source_model: Path,
    source_calibration_dir: Path,
    output_dir: Path,
    expected_count: int = DEFAULT_CALIBRATION_COUNT,
) -> dict[str, object]:
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError("onnxruntime is required to extract source features") from exc

    input_paths = _load_calibration_paths(source_calibration_dir, expected_count)
    exposed_model_path = output_dir / "source_features.onnx"
    _build_source_feature_model(source_model, exposed_model_path)
    session = ort.InferenceSession(
        str(exposed_model_path),
        providers=["CPUExecutionProvider"],
    )
    output_names = [spec.source_tensor for spec in SMOKE_SPECS]
    feature_records: dict[str, list[dict[str, object]]] = {
        spec.key: [] for spec in SMOKE_SPECS
    }

    for input_path in input_paths:
        source_input = np.load(input_path, allow_pickle=False)
        if source_input.shape != EXPECTED_SOURCE_INPUT_SHAPE:
            raise ValueError(
                f"invalid source calibration shape in {input_path}: "
                f"{source_input.shape}"
            )
        if source_input.dtype != np.float32:
            raise ValueError(
                f"invalid source calibration dtype in {input_path}: "
                f"{source_input.dtype}"
            )
        outputs = session.run(output_names, {SOURCE_INPUT_NAME: source_input})
        for spec, values in zip(SMOKE_SPECS, outputs):
            values = np.asarray(values, dtype=np.float32)
            if values.shape != spec.input_shape:
                raise ValueError(
                    f"runtime feature shape changed for {spec.key}: {values.shape}"
                )
            feature_path = output_dir / spec.key / "calibration_fp32" / input_path.name
            feature_path.parent.mkdir(parents=True, exist_ok=True)
            np.save(feature_path, values, allow_pickle=False)
            feature_records[spec.key].append(
                {
                    "source_input": input_path.name,
                    "source_input_sha256": sha256_file(input_path),
                    "feature_file": feature_path.as_posix(),
                    "feature_file_sha256": sha256_file(feature_path),
                    **_tensor_record(values),
                }
            )

    report = {
        "source_model": {
            "path": source_model.as_posix(),
            "bytes": source_model.stat().st_size,
            "sha256": sha256_file(source_model),
        },
        "exposed_feature_model": {
            "path": exposed_model_path.as_posix(),
            "bytes": exposed_model_path.stat().st_size,
            "sha256": sha256_file(exposed_model_path),
        },
        "source_calibration_directory": source_calibration_dir.as_posix(),
        "calibration_count": len(input_paths),
        "features": feature_records,
    }
    _write_json(output_dir / "feature_calibration_index.json", report)
    for spec in SMOKE_SPECS:
        _write_json(
            output_dir / spec.key / "calibration_fp32" / "calibration_index.json",
            {
                "schema_version": 1,
                "spec": spec.key,
                "source_tensor": spec.source_tensor,
                "shape": list(spec.input_shape),
                "count": len(feature_records[spec.key]),
                "tensors": feature_records[spec.key],
            },
        )
    return report


def _nearest_neighbor_reference(values: np.ndarray) -> np.ndarray:
    return np.repeat(np.repeat(values, 2, axis=2), 2, axis=3)


def verify_fp32_model(
    model_path: Path,
    spec: ConvTransposeSmokeSpec,
    calibration_dir: Path,
    expected_count: int = DEFAULT_CALIBRATION_COUNT,
) -> dict[str, object]:
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError("onnxruntime is required to verify smoke models") from exc

    input_paths = _load_calibration_paths(calibration_dir, expected_count)
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    maximum_absolute_difference = 0.0
    cases: list[dict[str, object]] = []
    for input_path in input_paths:
        values = np.load(input_path, allow_pickle=False)
        if values.shape != spec.input_shape or values.dtype != np.float32:
            raise ValueError(
                f"invalid {spec.key} feature tensor {input_path}: "
                f"shape={values.shape} dtype={values.dtype}"
            )
        output = np.asarray(session.run(["output"], {"input": values})[0])
        reference = _nearest_neighbor_reference(values)
        difference = float(np.max(np.abs(output - reference)))
        maximum_absolute_difference = max(maximum_absolute_difference, difference)
        cases.append(
            {
                "input_file": input_path.as_posix(),
                "input_sha256": sha256_file(input_path),
                "output": _tensor_record(output),
                "reference_tensor_sha256": tensor_sha256(reference),
                "maximum_absolute_difference": difference,
            }
        )
    exact = maximum_absolute_difference == 0.0
    return {
        "status": "passed" if exact else "failed",
        "model": {
            "path": model_path.as_posix(),
            "sha256": sha256_file(model_path),
        },
        "spec": spec.key,
        "calibration_count": len(cases),
        "reference": "np.repeat(axis=2,2) then np.repeat(axis=3,2)",
        "maximum_absolute_difference": maximum_absolute_difference,
        "bit_exact": exact,
        "cases": cases,
    }


def prepare_smoke_gate(
    source_model: Path,
    source_calibration_dir: Path,
    output_dir: Path,
    expected_count: int = DEFAULT_CALIBRATION_COUNT,
    expected_source_sha256: str = EXPECTED_SOURCE_MODEL_SHA256,
) -> dict[str, object]:
    if output_dir.exists():
        raise ValueError(f"output directory already exists: {output_dir}")
    if not source_model.is_file():
        raise ValueError(f"source model does not exist: {source_model}")
    actual_source_sha256 = sha256_file(source_model)
    if actual_source_sha256 != expected_source_sha256.lower():
        raise ValueError(
            "source model hash does not match the frozen QAT asset: "
            f"expected={expected_source_sha256.lower()} "
            f"actual={actual_source_sha256}"
        )
    if not source_calibration_dir.is_dir():
        raise ValueError(
            f"source calibration directory does not exist: {source_calibration_dir}"
        )

    feature_report = extract_feature_calibration(
        source_model,
        source_calibration_dir,
        output_dir,
        expected_count,
    )
    smoke_models: dict[str, dict[str, object]] = {}
    for spec in SMOKE_SPECS:
        model_path = output_dir / spec.key / f"convtranspose_{spec.key}.onnx"
        model_record = build_smoke_model(spec, model_path)
        fp32_report = verify_fp32_model(
            model_path,
            spec,
            output_dir / spec.key / "calibration_fp32",
            expected_count,
        )
        fp32_report_path = output_dir / spec.key / "fp32_gate.json"
        _write_json(fp32_report_path, fp32_report)
        smoke_models[spec.key] = {
            "spec": {
                "source_tensor": spec.source_tensor,
                "input_shape": list(spec.input_shape),
                "output_shape": list(spec.output_shape),
            },
            "model": model_record,
            "feature_calibration_dir": (
                output_dir / spec.key / "calibration_fp32"
            ).as_posix(),
            "fp32_gate": {
                "path": fp32_report_path.as_posix(),
                "bytes": fp32_report_path.stat().st_size,
                "sha256": sha256_file(fp32_report_path),
                "status": fp32_report["status"],
                "maximum_absolute_difference": fp32_report[
                    "maximum_absolute_difference"
                ],
            },
        }

    report = {
        "schema_version": 1,
        "status": (
            "fp32_prepared"
            if all(item["fp32_gate"]["status"] == "passed" for item in smoke_models.values())
            else "fp32_rejected"
        ),
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
        },
        "source": feature_report,
        "models": smoke_models,
    }
    _write_json(output_dir / "prepare_report.json", report)
    return report


def _numeric_comparison(
    actual: np.ndarray,
    reference: np.ndarray,
) -> dict[str, object]:
    difference = actual.astype(np.float64) - reference.astype(np.float64)
    rmse = float(np.sqrt(np.mean(np.square(difference))))
    reference_range = float(np.ptp(reference))
    normalized_rmse = rmse / max(reference_range, 1.0e-12)
    actual_flat = actual.astype(np.float64).ravel()
    reference_flat = reference.astype(np.float64).ravel()
    denominator = float(np.linalg.norm(actual_flat) * np.linalg.norm(reference_flat))
    cosine_similarity = (
        float(np.dot(actual_flat, reference_flat) / denominator)
        if denominator > 0.0
        else 1.0
    )
    return {
        "maximum_absolute_difference": float(np.max(np.abs(difference))),
        "mean_absolute_difference": float(np.mean(np.abs(difference))),
        "rmse": rmse,
        "normalized_rmse": normalized_rmse,
        "cosine_similarity": cosine_similarity,
        "actual": _tensor_record(actual),
        "reference": _tensor_record(reference),
    }


def run_saved_model_gate(
    model_path: Path,
    expected_model_sha256: str,
    spec: ConvTransposeSmokeSpec,
    calibration_dir: Path,
    output_dir: Path,
    expected_count: int = DEFAULT_CALIBRATION_COUNT,
) -> dict[str, object]:
    if output_dir.exists():
        raise ValueError(f"output directory already exists: {output_dir}")
    if sha256_file(model_path) != expected_model_sha256.lower():
        raise ValueError("saved model hash does not match the expected hash")
    input_paths = _load_calibration_paths(calibration_dir, expected_count)

    try:
        import mera
        from mera import Platform, Target
    except ImportError as exc:
        raise RuntimeError("the pinned MERA environment is required") from exc

    started = time.perf_counter()
    deployment_dir = output_dir / "deployment"
    with mera.Deployer(str(deployment_dir), overwrite=False) as deployer:
        model = mera.ModelLoader(deployer).from_quantized_mera(str(model_path))
        input_names = sorted(model.input_desc.all_inputs)
        if len(input_names) != 1:
            raise ValueError(f"saved model must expose one input: {input_names}")
        deployment = deployer.deploy(
            model,
            mera_platform=Platform.MCU_CPU,
            target=Target.MERAInterpreter,
        )
        runner = deployment.get_runner()
        cases: list[dict[str, object]] = []
        for input_path in input_paths:
            values = np.load(input_path, allow_pickle=False)
            if values.shape != spec.input_shape or values.dtype != np.float32:
                raise ValueError(f"invalid saved-gate input: {input_path}")
            runner.set_input({input_names[0]: values}).run()
            outputs = runner.get_outputs_dict()
            if len(outputs) != 1:
                raise ValueError(
                    f"saved model must expose one output: {sorted(outputs)}"
                )
            output_name, actual = next(iter(outputs.items()))
            actual = np.asarray(actual, dtype=np.float32)
            reference = _nearest_neighbor_reference(values)
            comparison = _numeric_comparison(actual, reference)
            case_acceptance = {
                "shape_matches": actual.shape == spec.output_shape,
                "finite": bool(np.all(np.isfinite(actual))),
                "noncollapsed": (
                    float(np.ptp(reference)) == 0.0 or float(np.ptp(actual)) > 0.0
                ),
                "normalized_rmse_at_most_0_10": (
                    comparison["normalized_rmse"] <= 0.10
                ),
                "cosine_similarity_at_least_0_95": (
                    comparison["cosine_similarity"] >= 0.95
                ),
            }
            cases.append(
                {
                    "input_file": input_path.as_posix(),
                    "input_sha256": sha256_file(input_path),
                    "output_name": output_name,
                    "comparison": comparison,
                    "acceptance": case_acceptance,
                    "passed": all(case_acceptance.values()),
                }
            )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    qualified = all(case["passed"] for case in cases)
    report = {
        "schema_version": 1,
        "status": "saved_model_qualified" if qualified else "saved_model_rejected",
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "mera": mera.__version__,
            "loader": "ModelLoader.from_quantized_mera",
            "deployment_platform": "Platform.MCU_CPU",
            "deployment_target": "Target.MERAInterpreter",
        },
        "spec": spec.key,
        "saved_model": {
            "path": model_path.as_posix(),
            "bytes": model_path.stat().st_size,
            "sha256": sha256_file(model_path),
        },
        "input_name": input_names[0],
        "calibration_count": len(cases),
        "elapsed_ms": elapsed_ms,
        "acceptance_thresholds": {
            "normalized_rmse_maximum": 0.10,
            "cosine_similarity_minimum": 0.95,
        },
        "worst_normalized_rmse": max(
            case["comparison"]["normalized_rmse"] for case in cases
        ),
        "minimum_cosine_similarity": min(
            case["comparison"]["cosine_similarity"] for case in cases
        ),
        "cases": cases,
    }
    _write_json(output_dir / "saved_model_gate.json", report)
    return report


def _contains_learned_operator(value: str) -> bool:
    lowered = value.lower()
    return any(token.lower() in lowered for token in LEARNED_OPERATOR_TOKENS)


def _learned_operator_tokens(value: str) -> list[str]:
    lowered = value.lower()
    return [
        token
        for token in LEARNED_OPERATOR_TOKENS
        if token.lower() in lowered
    ]


def _find_mcu_subgraph_report(compiler_output_dir: Path) -> Path:
    matches = sorted(compiler_output_dir.rglob("model_subgraphs.json"))
    deploy_matches = [
        path for path in matches if "deploy" in {part.lower() for part in path.parts}
    ]
    candidates = deploy_matches or matches
    if len(candidates) != 1:
        raise ValueError(
            "expected one deployed MCU model_subgraphs.json, found "
            f"{[path.as_posix() for path in candidates]}"
        )
    return candidates[0]


def inspect_compiler_allocation(
    compiler_output_dir: Path,
    output_report_path: Path,
) -> dict[str, object]:
    subgraph_path = _find_mcu_subgraph_report(compiler_output_dir)
    document = json.loads(subgraph_path.read_text(encoding="utf-8"))
    regions: list[dict[str, object]] = []
    for index, subgraph in enumerate(document.get("subgraphs", [])):
        operators = sorted(
            {
                str(node.get("op_name", ""))
                for node in subgraph.get("nodes", [])
                if node.get("op_name")
            }
        )
        regions.append(
            {
                "index": index,
                "target": str(subgraph.get("target_name", "UNKNOWN")),
                "operators": operators,
                "learned_operators": [
                    operator
                    for operator in operators
                    if _contains_learned_operator(operator)
                ],
            }
        )

    cpu_region_dirs = sorted(
        path
        for path in compiler_output_dir.rglob("sub_*__CPU_C_CODEGEN")
        if path.is_dir()
    )
    cpu_region_sources: list[dict[str, object]] = []
    for directory in cpu_region_dirs:
        source_files = {
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in {".c", ".h", ".json", ".txt"}
        }
        subgraph_id = directory.name.split("__", 1)[0].removeprefix("sub_")
        generated_source_root = directory.parent / "src"
        source_files.update(generated_source_root.glob(f"compute_sub_{subgraph_id}.*"))
        matches: list[dict[str, object]] = []
        source_records: list[dict[str, object]] = []
        for source_path in sorted(source_files):
            if not source_path.is_file():
                continue
            text = source_path.read_text(encoding="utf-8", errors="replace")
            tokens = _learned_operator_tokens(text)
            source_records.append(
                {
                    "path": source_path.as_posix(),
                    "bytes": source_path.stat().st_size,
                    "sha256": sha256_file(source_path),
                }
            )
            if tokens:
                matches.append(
                    {
                        "path": source_path.as_posix(),
                        "sha256": sha256_file(source_path),
                        "tokens": tokens,
                    }
                )
        cpu_region_sources.append(
            {
                "path": directory.as_posix(),
                "generated_sources": source_records,
                "learned_operator_source_matches": matches,
            }
        )

    npu_regions = [
        region for region in regions if "ARM_ETHOS_U55" in region["target"]
    ]
    cpu_regions = [
        region
        for region in regions
        if region["target"] == "C_CODEGEN" or "CPU" in region["target"]
    ]
    npu_learned = [
        region for region in npu_regions if region["learned_operators"]
    ]
    cpu_learned = [
        region for region in cpu_regions if region["learned_operators"]
    ]
    cpu_source_learned = [
        region
        for region in cpu_region_sources
        if region["learned_operator_source_matches"]
    ]
    model_paths = sorted(compiler_output_dir.rglob("model.mera"))
    generated_model_sources = sorted(compiler_output_dir.rglob("model.c"))
    acceptance = {
        "saved_model_exists": bool(model_paths),
        "generated_model_c_exists": bool(generated_model_sources),
        "npu_region_executes_convtranspose": bool(npu_learned),
        "cpu_subgraph_does_not_execute_convtranspose": not cpu_learned,
        "cpu_generated_source_does_not_execute_convtranspose": (
            not cpu_learned and not cpu_source_learned
        ),
    }
    report = {
        "schema_version": 1,
        "status": "allocation_qualified" if all(acceptance.values()) else "allocation_rejected",
        "compiler_output_dir": compiler_output_dir.as_posix(),
        "model_subgraphs": {
            "path": subgraph_path.as_posix(),
            "bytes": subgraph_path.stat().st_size,
            "sha256": sha256_file(subgraph_path),
        },
        "regions": regions,
        "npu_region_count": len(npu_regions),
        "cpu_region_count": len(cpu_regions),
        "npu_learned_operator_region_count": len(npu_learned),
        "cpu_learned_operator_region_count": len(cpu_learned),
        "cpu_region_sources": cpu_region_sources,
        "saved_models": [
            {
                "path": path.as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for path in model_paths
        ],
        "generated_model_sources": [
            {
                "path": path.as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for path in generated_model_sources
        ],
        "acceptance": acceptance,
    }
    _write_json(output_report_path, report)
    return report


def _print_summary(prefix: str, report: dict[str, object]) -> None:
    print(f"{prefix} status={report['status']}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prepare and qualify fixed-shape RA8P1 ConvTranspose smoke models"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare")
    prepare.add_argument("--source-model", required=True, type=Path)
    prepare.add_argument("--source-calibration-dir", required=True, type=Path)
    prepare.add_argument("--output-dir", required=True, type=Path)

    saved = subparsers.add_parser("saved-gate")
    saved.add_argument("--model", required=True, type=Path)
    saved.add_argument("--expected-model-sha256", required=True)
    saved.add_argument("--spec", required=True, choices=[spec.key for spec in SMOKE_SPECS])
    saved.add_argument("--calibration-dir", required=True, type=Path)
    saved.add_argument("--output-dir", required=True, type=Path)

    allocation = subparsers.add_parser("inspect-allocation")
    allocation.add_argument("--compiler-output-dir", required=True, type=Path)
    allocation.add_argument("--output-report", required=True, type=Path)

    args = parser.parse_args(argv)
    if args.command == "prepare":
        report = prepare_smoke_gate(
            args.source_model.resolve(),
            args.source_calibration_dir.resolve(),
            args.output_dir.resolve(),
        )
        _print_summary("convtranspose-prepare", report)
        return 0 if report["status"] == "fp32_prepared" else 2
    if args.command == "saved-gate":
        report = run_saved_model_gate(
            args.model.resolve(),
            args.expected_model_sha256,
            get_smoke_spec(args.spec),
            args.calibration_dir.resolve(),
            args.output_dir.resolve(),
        )
        _print_summary("convtranspose-saved-gate", report)
        return 0 if report["status"] == "saved_model_qualified" else 3
    if args.command == "inspect-allocation":
        report = inspect_compiler_allocation(
            args.compiler_output_dir.resolve(),
            args.output_report.resolve(),
        )
        _print_summary("convtranspose-allocation", report)
        return 0 if report["status"] == "allocation_qualified" else 4
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
