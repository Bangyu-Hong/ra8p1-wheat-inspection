from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np


TOOLS_ROOT = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))

from wheat_deployment.yolox_tile224_gate import (  # noqa: E402
    EXPECTED_MODEL_SHA256,
    _is_ascii_path,
    compare_int8_raw_output,
    inspect_compiler_output,
    prepare_ascii_staging,
    validate_runtime_tflite,
    verify_provenance,
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class _FakeInterpreter:
    def __init__(self, *, model_path: str) -> None:
        self.model_path = model_path
        self.allocated = False

    def allocate_tensors(self) -> None:
        self.allocated = True

    def get_input_details(self):
        return [
            {
                "name": "serving_default_images:0",
                "shape": np.asarray([1, 224, 224, 3], dtype=np.int32),
                "dtype": np.int8,
                "quantization": (1.0, -128),
            }
        ]

    def get_output_details(self):
        return [
            {
                "name": "PartitionedCall:0",
                "shape": np.asarray([1, 1029, 6], dtype=np.int32),
                "dtype": np.int8,
                "quantization": (0.03774220868945122, -48),
            }
        ]


class YoloxTile224GateTests(unittest.TestCase):
    def test_provenance_verifies_every_member_and_detects_corruption(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "one.bin"
            second = root / "nested/two.bin"
            second.parent.mkdir()
            first.write_bytes(b"one")
            second.write_bytes(b"two")
            provenance = {
                "files": [
                    {"path": "one.bin", "bytes": 3, "sha256": _sha256(first)},
                    {
                        "path": "nested/two.bin",
                        "bytes": 3,
                        "sha256": _sha256(second),
                    },
                ]
            }
            (root / "provenance.json").write_text(
                json.dumps(provenance), encoding="utf-8"
            )

            passed = verify_provenance(root, expected_entries=2)
            self.assertEqual(passed["status"], "passed")
            self.assertEqual(passed["verified_entries"], 2)

            second.write_bytes(b"bad")
            failed = verify_provenance(root, expected_entries=2)
            self.assertEqual(failed["status"], "failed")
            self.assertTrue(any("sha256 mismatch" in item for item in failed["failures"]))

    def test_runtime_contract_reads_actual_interpreter_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.tflite"
            model.write_bytes(b"fake-flatbuffer")
            result = validate_runtime_tflite(
                model, interpreter_factory=_FakeInterpreter
            )
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["input"]["shape"], [1, 224, 224, 3])
        self.assertEqual(result["output"]["shape"], [1, 1029, 6])

    def test_runtime_contract_rejects_wrong_quantization(self) -> None:
        class WrongInput(_FakeInterpreter):
            def get_input_details(self):
                details = super().get_input_details()
                details[0]["quantization"] = (0.5, -128)
                return details

        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.tflite"
            model.write_bytes(b"fake-flatbuffer")
            result = validate_runtime_tflite(model, interpreter_factory=WrongInput)
        self.assertEqual(result["status"], "failed")
        self.assertTrue(any("scale mismatch" in item for item in result["failures"]))

    def test_ascii_staging_is_hash_keyed_and_reusable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.tflite"
            source.write_bytes(b"frozen-model")
            expected = _sha256(source)
            staging = root / "staging"
            created = prepare_ascii_staging(
                source, staging, expected_sha256=expected
            )
            reused = prepare_ascii_staging(
                source, staging, expected_sha256=expected
            )
        self.assertEqual(created["status"], "passed")
        self.assertEqual(created["action"], "created")
        self.assertEqual(reused["action"], "reused")
        self.assertEqual(created["staged_sha256"], expected)

    def test_non_ascii_staging_is_rejected_before_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.tflite"
            source.write_bytes(b"frozen-model")
            result = prepare_ascii_staging(
                source,
                root / "模型",
                expected_sha256=_sha256(source),
            )
        self.assertEqual(result["status"], "failed")
        self.assertFalse(_is_ascii_path(Path(result["destination"])))
        self.assertTrue(any("not ASCII" in item for item in result["failures"]))

    def test_official_hash_constant_is_frozen(self) -> None:
        self.assertEqual(
            EXPECTED_MODEL_SHA256,
            "7f6a058eb5b4cc4cc61942cdac55a6645fbffe9992731f1f6d72d897338f07f3",
        )

    def test_allocation_accepts_focus_only_but_blocks_flash_overflow(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "compiler"
            build = compiler / "model_NPU/deploy/build/MCU"
            compilation = build / "compilation"
            source = compilation / "src"
            vela = compilation / "sub_0004__ARM_ETHOS_U55_C_CODEGEN"
            source.mkdir(parents=True)
            vela.mkdir()

            subgraphs = {"subgraphs": []}
            for index in range(4):
                subgraphs["subgraphs"].append(
                    {
                        "subgraph_name": f"sub_{index}",
                        "target_name": "C_CODEGEN",
                        "nodes": [
                            {"op_name": "Int32VecConstant"},
                            {"op_name": "TFLiteStridedSlice"},
                            {"op_name": "OutputNode"},
                        ],
                    }
                )
            subgraphs["subgraphs"].append(
                {
                    "subgraph_name": "sub_4",
                    "target_name": "ARM_ETHOS_U55",
                    "nodes": [
                        {"op_name": "TFLiteQConv2dBias"},
                        {"op_name": "Upsampling"},
                        {"op_name": "Upsampling"},
                        {"op_name": "OutputNode"},
                    ],
                }
            )
            (build / "model_subgraphs.json").write_text(
                json.dumps(subgraphs), encoding="utf-8"
            )
            (build / "io_desc.json").write_text(
                json.dumps(
                    {
                        "inputs": [["serving_default_images_0", ["1", "224", "224", "3"], "Int8"]],
                        "outputs": [["PartitionedCall_0_70473", ["1", "1029", "6"], "Int8"]],
                    }
                ),
                encoding="utf-8",
            )
            (build / "deploy_cfg.json").write_text(
                json.dumps(
                    {"backend_options": {"ccodegen": {"enable_ospi": False}}}
                ),
                encoding="utf-8",
            )
            (vela / "model_summary_RA8P1.csv").write_text(
                "memory_mode,weights_storage_area,feature_map_storage_area,"
                "on_chip_flash_memory_used,off_chip_flash_memory_used,"
                "sram_memory_used,inference_time\n"
                "Sram_Only,On-chip Flash,SRAM,4541.09375,0.0,784.0,0.02588966\n",
                encoding="utf-8",
            )
            (source / "model.c").write_text(
                "int8_t buf_input[150528];\n"
                "int8_t buf_focus_0[37632];\n"
                "int8_t buf_focus_1[37632];\n"
                "int8_t buf_focus_2[37632];\n"
                "int8_t buf_focus_3[37632];\n",
                encoding="utf-8",
            )
            (source / "sub_0004_model_data.c").write_text(
                "const size_t sub_0004_model_data_size = 4625552;\n",
                encoding="utf-8",
            )
            (source / "sub_0004_invoke.c").write_text(
                "__attribute__((aligned(16))) uint8_t sub_0004_arena[802816];\n",
                encoding="utf-8",
            )
            for index in range(4):
                (source / f"compute_sub_{index:04d}.h").write_text(
                    f"kBufferSize_sub_{index:04d} = 9\n", encoding="utf-8"
                )
            memory_regions = root / "memory_regions.ld"
            memory_regions.write_text(
                "FLASH_LENGTH = 0x00100000;\n", encoding="utf-8"
            )
            bsp_config = root / "bsp_mcu_family_cfg.h"
            bsp_config.write_text(
                "#define BSP_CFG_OSPI_B_STARTUP_ENABLED (0)\n", encoding="utf-8"
            )

            report = inspect_compiler_output(
                compiler, memory_regions, firmware_bsp_config=bsp_config
            )

        self.assertEqual(report["status"], "host_gate_blocked")
        self.assertFalse(report["firmware_integration_allowed"])
        self.assertTrue(
            all(report["allocation"]["operator_acceptance"].values())
        )
        self.assertFalse(
            report["memory"]["acceptance"][
                "generated_constants_fit_selected_storage"
            ]
        )
        self.assertEqual(
            report["memory"]["generated_mutable_buffers"][
                "static_mutable_bytes_lower_bound"
            ],
            1103908,
        )

    def test_allocation_rejects_cpu_convolution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "compiler"
            build = compiler / "build"
            source = build / "compilation/src"
            vela = build / "compilation/npu"
            source.mkdir(parents=True)
            vela.mkdir()
            (build / "model_subgraphs.json").write_text(
                json.dumps(
                    {
                        "subgraphs": [
                            {
                                "target_name": "C_CODEGEN",
                                "nodes": [{"op_name": "TFLiteQConv2dBias"}],
                            },
                            {
                                "target_name": "ARM_ETHOS_U55",
                                "nodes": [
                                    {"op_name": "TFLiteQConv2dBias"},
                                    {"op_name": "Upsampling"},
                                    {"op_name": "Upsampling"},
                                ],
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (build / "io_desc.json").write_text(
                json.dumps(
                    {
                        "inputs": [["serving_default_images_0", ["1", "224", "224", "3"], "Int8"]],
                        "outputs": [["PartitionedCall_0_70473", ["1", "1029", "6"], "Int8"]],
                    }
                ),
                encoding="utf-8",
            )
            (build / "deploy_cfg.json").write_text(
                json.dumps(
                    {"backend_options": {"ccodegen": {"enable_ospi": False}}}
                ),
                encoding="utf-8",
            )
            (vela / "model_summary_RA8P1.csv").write_text(
                "memory_mode,weights_storage_area,feature_map_storage_area,"
                "on_chip_flash_memory_used,off_chip_flash_memory_used,"
                "sram_memory_used,inference_time\n"
                "Sram_Only,On-chip Flash,SRAM,100,0,100,0.01\n",
                encoding="utf-8",
            )
            (source / "model.c").write_text("int8_t buf_input[1];\n", encoding="utf-8")
            (source / "sub_0001_model_data.c").write_text(
                "const size_t sub_0001_model_data_size = 100;\n", encoding="utf-8"
            )
            memory_regions = root / "memory_regions.ld"
            memory_regions.write_text("FLASH_LENGTH = 0x00100000;\n", encoding="utf-8")
            bsp_config = root / "bsp_mcu_family_cfg.h"
            bsp_config.write_text(
                "#define BSP_CFG_OSPI_B_STARTUP_ENABLED (0)\n", encoding="utf-8"
            )

            report = inspect_compiler_output(
                compiler, memory_regions, firmware_bsp_config=bsp_config
            )

        self.assertEqual(report["status"], "compiler_output_rejected")
        self.assertFalse(
            report["allocation"]["operator_acceptance"]["cpu_regions_are_focus_only"]
        )

    def test_raw_output_comparison_allows_one_quantized_step(self) -> None:
        expected = np.zeros((1, 1029, 6), dtype=np.int8)
        expected[..., 4] = np.arange(1029, dtype=np.int16).reshape(1, 1029) % 127
        actual = expected.copy()
        actual[0, 7, 4] += 1

        result = compare_int8_raw_output(actual, expected)

        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["maximum_absolute_error"], 1)
        self.assertEqual(result["changed_values"], 1)

    def test_raw_output_comparison_rejects_collapse(self) -> None:
        expected = np.ones((1, 1029, 6), dtype=np.int8)
        actual = np.zeros((1, 1029, 6), dtype=np.int8)

        result = compare_int8_raw_output(actual, expected)

        self.assertEqual(result["status"], "failed")
        self.assertFalse(result["nonconstant"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
