"""Host tests for the fixed-shape ConvTranspose deployment gate."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np


TOOLS_ROOT = Path(__file__).resolve().parents[1] / "tools"
REPOSITORY_ROOT = TOOLS_ROOT.parents[2]
sys.path.insert(0, str(TOOLS_ROOT))

from wheat_deployment.convtranspose_smoke import (  # noqa: E402
    ConvTransposeSmokeSpec,
    SMOKE_SPECS,
    build_smoke_model,
    extract_feature_calibration,
    inspect_compiler_allocation,
    prepare_smoke_gate,
    sha256_file,
    verify_fp32_model,
)


class ConvTransposeModelTests(unittest.TestCase):
    def test_model_is_static_diagonal_and_bit_exact(self) -> None:
        try:
            import onnx
            from onnx import numpy_helper
        except ImportError:
            self.skipTest("onnx is not installed")

        spec = ConvTransposeSmokeSpec(
            key="test_c3_2_to_4",
            source_tensor="feature",
            channels=3,
            input_size=2,
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            model_path = root / "model.onnx"
            calibration_dir = root / "calibration"
            calibration_dir.mkdir()
            values = np.arange(12, dtype=np.float32).reshape(1, 3, 2, 2)
            np.save(calibration_dir / "input.npy", values, allow_pickle=False)

            record = build_smoke_model(spec, model_path)
            gate = verify_fp32_model(
                model_path,
                spec,
                calibration_dir,
                expected_count=1,
            )
            model = onnx.load(model_path)
            weights = numpy_helper.to_array(
                next(item for item in model.graph.initializer if item.name == "weight")
            )

            self.assertEqual([node.op_type for node in model.graph.node], ["ConvTranspose"])
            self.assertEqual(record["input_shape"], [1, 3, 2, 2])
            self.assertEqual(record["output_shape"], [1, 3, 4, 4])
            self.assertEqual(record["weight_nonzero_count"], 12)
            self.assertTrue(np.all(weights[0, 0] == 1.0))
            self.assertTrue(np.all(weights[0, 1:] == 0.0))
            self.assertEqual(gate["status"], "passed")
            self.assertTrue(gate["bit_exact"])
            self.assertEqual(gate["maximum_absolute_difference"], 0.0)

    def test_real_smoke_specs_freeze_the_two_yolo_shapes(self) -> None:
        self.assertEqual(
            [
                (spec.source_tensor, spec.input_shape, spec.output_shape)
                for spec in SMOKE_SPECS
            ],
            [
                (
                    "/model.9/cv2/act/Mul_output_0",
                    (1, 256, 10, 10),
                    (1, 256, 20, 20),
                ),
                (
                    "/model.12/cv2/act/Mul_output_0",
                    (1, 128, 20, 20),
                    (1, 128, 40, 40),
                ),
            ],
        )

    def test_formal_prepare_rejects_a_different_source_asset(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source.onnx"
            source_path.write_bytes(b"not-the-frozen-qat-model")
            calibration_dir = root / "calibration"
            calibration_dir.mkdir()

            with self.assertRaisesRegex(ValueError, "source model hash"):
                prepare_smoke_gate(
                    source_path,
                    calibration_dir,
                    root / "output",
                )

    def test_zero_calibration_count_cannot_pass(self) -> None:
        try:
            import onnx  # noqa: F401
        except ImportError:
            self.skipTest("onnx is not installed")

        spec = ConvTransposeSmokeSpec(
            key="test_c1_1_to_2",
            source_tensor="feature",
            channels=1,
            input_size=1,
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            model_path = root / "model.onnx"
            calibration_dir = root / "calibration"
            calibration_dir.mkdir()
            build_smoke_model(spec, model_path)

            with self.assertRaisesRegex(ValueError, "must be positive"):
                verify_fp32_model(
                    model_path,
                    spec,
                    calibration_dir,
                    expected_count=0,
                )

    def test_feature_extraction_uses_source_activations(self) -> None:
        try:
            import onnx
            from onnx import TensorProto, helper, numpy_helper
        except ImportError:
            self.skipTest("onnx is not installed")

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source.onnx"
            calibration_dir = root / "source_calibration"
            calibration_dir.mkdir()
            output_dir = root / "features"
            source_input = np.zeros((1, 3, 320, 320), dtype=np.float32)
            np.save(calibration_dir / "case.npy", source_input, allow_pickle=False)

            feature_values = [
                np.full(spec.input_shape, index + 1, dtype=np.float32)
                for index, spec in enumerate(SMOKE_SPECS)
            ]
            nodes = [
                helper.make_node(
                    "Constant",
                    inputs=[],
                    outputs=[spec.source_tensor],
                    value=numpy_helper.from_array(values),
                )
                for spec, values in zip(SMOKE_SPECS, feature_values)
            ]
            graph = helper.make_graph(
                nodes,
                "feature_fixture",
                [
                    helper.make_tensor_value_info(
                        "images",
                        TensorProto.FLOAT,
                        [1, 3, 320, 320],
                    )
                ],
                [
                    helper.make_tensor_value_info(
                        SMOKE_SPECS[-1].source_tensor,
                        TensorProto.FLOAT,
                        SMOKE_SPECS[-1].input_shape,
                    )
                ],
            )
            model = helper.make_model(
                graph,
                opset_imports=[helper.make_opsetid("", 13)],
            )
            model.ir_version = 7
            onnx.save(model, source_path)

            report = extract_feature_calibration(
                source_path,
                calibration_dir,
                output_dir,
                expected_count=1,
            )

            self.assertEqual(report["calibration_count"], 1)
            for spec, expected in zip(SMOKE_SPECS, feature_values):
                saved = np.load(
                    output_dir / spec.key / "calibration_fp32" / "case.npy",
                    allow_pickle=False,
                )
                np.testing.assert_array_equal(saved, expected)


class ConvTransposeAllocationTests(unittest.TestCase):
    def _make_compiler_fixture(
        self,
        root: Path,
        cpu_learned: bool,
        cpu_source_text: str | None = None,
    ) -> Path:
        compiler_root = root / "compiler"
        mcu_root = compiler_root / "candidate" / "deploy" / "build" / "MCU"
        cpu_dir = mcu_root / "compilation" / "sub_0000__CPU_C_CODEGEN"
        npu_dir = mcu_root / "compilation" / "sub_0001__ARM_ETHOS_U55_C_CODEGEN"
        source_dir = mcu_root / "compilation" / "src"
        cpu_dir.mkdir(parents=True)
        npu_dir.mkdir(parents=True)
        source_dir.mkdir(parents=True)
        (compiler_root / "candidate" / "quantization").mkdir(parents=True)
        (compiler_root / "candidate" / "quantization" / "model.mera").write_bytes(
            b"model"
        )
        (source_dir / "model.c").write_text("void model(void) {}\n", encoding="utf-8")
        (cpu_dir / "compute.c").write_text(
            cpu_source_text
            if cpu_source_text is not None
            else ("ConvTranspose();\n" if cpu_learned else "QuantizeInput();\n"),
            encoding="utf-8",
        )
        cpu_operators = ["InputNode", "ConvTranspose"] if cpu_learned else ["InputNode"]
        subgraphs = {
            "subgraphs": [
                {
                    "target_name": "CPU_C_CODEGEN",
                    "nodes": [{"op_name": name} for name in cpu_operators],
                },
                {
                    "target_name": "ARM_ETHOS_U55",
                    "nodes": [
                        {"op_name": "TFLiteTransposeConv"},
                        {"op_name": "OutputNode"},
                    ],
                },
            ]
        }
        (mcu_root / "model_subgraphs.json").write_text(
            json.dumps(subgraphs),
            encoding="utf-8",
        )
        return compiler_root

    def test_allocation_passes_only_for_npu_learned_operator(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            compiler_root = self._make_compiler_fixture(root, cpu_learned=False)
            report = inspect_compiler_allocation(
                compiler_root,
                root / "allocation.json",
            )

            self.assertEqual(report["status"], "allocation_qualified")
            self.assertEqual(report["npu_learned_operator_region_count"], 1)
            self.assertEqual(report["cpu_learned_operator_region_count"], 0)
            self.assertTrue(all(report["acceptance"].values()))

    def test_allocation_rejects_cpu_convtranspose(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            compiler_root = self._make_compiler_fixture(root, cpu_learned=True)
            report = inspect_compiler_allocation(
                compiler_root,
                root / "allocation.json",
            )

            self.assertEqual(report["status"], "allocation_rejected")
            self.assertEqual(report["cpu_learned_operator_region_count"], 1)
            self.assertFalse(
                report["acceptance"]["cpu_subgraph_does_not_execute_convtranspose"]
            )
            self.assertFalse(
                report["acceptance"][
                    "cpu_generated_source_does_not_execute_convtranspose"
                ]
            )

    def test_allocation_source_scan_is_case_insensitive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            compiler_root = self._make_compiler_fixture(
                root,
                cpu_learned=False,
                cpu_source_text="transposeconv();\n",
            )
            report = inspect_compiler_allocation(
                compiler_root,
                root / "allocation.json",
            )

            self.assertEqual(report["status"], "allocation_rejected")
            self.assertFalse(
                report["acceptance"][
                    "cpu_generated_source_does_not_execute_convtranspose"
                ]
            )


class ConvTransposeGateManifestTests(unittest.TestCase):
    def test_rejection_manifest_freezes_allocation_failure(self) -> None:
        manifest_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "convtranspose_operator_gate.json"
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(manifest["status"], "rejected")
        self.assertEqual(len(manifest["shapes"]), 2)
        self.assertTrue(
            manifest["decision"]["saved_model_numeric_gate_passed_for_both_shapes"]
        )
        self.assertFalse(manifest["decision"]["operator_gate_passed"])
        self.assertFalse(
            manifest["decision"]["no_resize_yolov8n_training_unlocked"]
        )
        self.assertIn("NanoDet-Plus", manifest["decision"]["next_route"])

        for shape in manifest["shapes"]:
            self.assertEqual(shape["fp32_gate"]["status"], "passed")
            self.assertEqual(
                shape["fp32_gate"]["maximum_absolute_difference_from_np_repeat"],
                0.0,
            )
            self.assertEqual(shape["saved_model_gate"]["status"], "passed")
            allocation = shape["allocation_gate"]
            self.assertEqual(allocation["status"], "rejected")
            self.assertEqual(allocation["c_codegen_learned_operator_region_count"], 1)
            self.assertEqual(allocation["arm_ethos_u55_learned_operator_region_count"], 0)
            self.assertEqual(
                allocation["learned_operator"],
                "TFLiteTransposeConv2dBias",
            )
            self.assertEqual(
                allocation["npu_region_operators"],
                ["TFLiteTranspose", "TFLiteTranspose"],
            )

        artifacts = [
            manifest["immutable_source"],
            manifest["calibration"]["prepare_report"],
        ]
        for shape in manifest["shapes"]:
            artifacts.extend(
                [
                    shape["smoke_onnx"],
                    shape["fp32_gate"],
                    shape["compiler"]["log"],
                    shape["compiler"]["saved_model"],
                    shape["saved_model_gate"],
                    shape["allocation_gate"]["generated_cpu_kernel"],
                    shape["allocation_gate"]["model_subgraphs"],
                    shape["allocation_gate"]["report"],
                ]
            )
        for artifact in artifacts:
            path = REPOSITORY_ROOT / artifact["generated_path"]
            if not path.is_file():
                continue
            self.assertEqual(path.stat().st_size, artifact["bytes"])
            self.assertEqual(sha256_file(path), artifact["sha256"])


if __name__ == "__main__":
    unittest.main()
