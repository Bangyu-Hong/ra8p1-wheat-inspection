"""Independent host checks for the wheat deployment reference."""

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

from wheat_deployment.contract import (  # noqa: E402
    load_model_contract,
    sha256_file,
    verify_model_contract,
)
from wheat_deployment.manifest import (  # noqa: E402
    build_gwhd_manifest,
    load_gwhd_vga_rgb,
    verify_gwhd_manifest,
)
from wheat_deployment.postprocess import (  # noqa: E402
    Detection,
    ResultCapacityError,
    assemble_six_head_logits,
    assemble_yolo_output,
    build_yolov8_anchor_grid,
    decode_yolo11_output,
    global_nms,
    reconstruct_yolov8_output,
)
from wheat_deployment.preprocess import (  # noqa: E402
    build_fp32_model_input,
    letterbox_vga_rgb,
    quantize_model_input,
    rgb565_be_to_rgb888,
)
from wheat_deployment.qualification import (  # noqa: E402
    qualify_predecode_candidate,
    qualify_six_head_candidate,
)


class PreprocessTests(unittest.TestCase):
    def test_letterbox_is_exact_and_does_not_resize_vga(self) -> None:
        rows = np.arange(480, dtype=np.uint16)[:, None]
        columns = np.arange(640, dtype=np.uint16)[None, :]
        rgb = np.empty((480, 640, 3), dtype=np.uint8)
        rgb[:, :, 0] = rows % 256
        rgb[:, :, 1] = columns % 256
        rgb[:, :, 2] = (rows + columns) % 256

        result = letterbox_vga_rgb(rgb)

        self.assertEqual(result.shape, (640, 640, 3))
        self.assertEqual(result.dtype, np.uint8)
        self.assertTrue(np.all(result[:80] == 114))
        self.assertTrue(np.array_equal(result[80:560], rgb))
        self.assertTrue(np.all(result[560:] == 114))

    def test_fp32_input_is_rgb_nchw_normalized(self) -> None:
        rgb = np.zeros((480, 640, 3), dtype=np.uint8)
        rgb[0, 0] = (255, 128, 0)

        result = build_fp32_model_input(rgb)

        self.assertEqual(result.shape, (1, 3, 640, 640))
        self.assertEqual(result.dtype, np.float32)
        np.testing.assert_array_equal(
            result[0, :, 80, 0],
            np.asarray([255, 128, 0], dtype=np.float32) / np.float32(255.0),
        )
        np.testing.assert_array_equal(
            result[0, :, 0, 0],
            np.full(3, 114, dtype=np.float32) / np.float32(255.0),
        )

    def test_full320_preprocessing_resizes_vga_then_pads_40_rows(self) -> None:
        rgb = np.empty((480, 640, 3), dtype=np.uint8)
        rgb[:, :, :] = [16, 96, 240]

        result = build_fp32_model_input(rgb, model_size=320)

        self.assertEqual(result.shape, (1, 3, 320, 320))
        self.assertEqual(result.dtype, np.float32)
        np.testing.assert_array_equal(
            result[0, :, 39, 0],
            np.full(3, 114, dtype=np.float32) / np.float32(255.0),
        )
        np.testing.assert_array_equal(
            result[0, :, 40, 0],
            np.asarray([16, 96, 240], dtype=np.float32) / np.float32(255.0),
        )
        np.testing.assert_array_equal(
            result[0, :, 279, 319],
            np.asarray([16, 96, 240], dtype=np.float32) / np.float32(255.0),
        )
        np.testing.assert_array_equal(
            result[0, :, 280, 319],
            np.full(3, 114, dtype=np.float32) / np.float32(255.0),
        )

    def test_rgb565_big_endian_primary_colors(self) -> None:
        frame = bytearray(1280 * 480)
        frame[0:8] = bytes((0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0x18, 0xA7))

        result = rgb565_be_to_rgb888(frame)

        np.testing.assert_array_equal(result[0, 0], [255, 0, 0])
        np.testing.assert_array_equal(result[0, 1], [0, 255, 0])
        np.testing.assert_array_equal(result[0, 2], [0, 0, 255])
        np.testing.assert_array_equal(result[0, 3], [24, 20, 57])

    def test_quantization_rounding_and_saturation(self) -> None:
        tensor = np.zeros((1, 3, 640, 640), dtype=np.float32)
        tensor[0, 0, 0, :5] = [-2.0, -0.5, 0.0, 0.5, 20.0]

        result = quantize_model_input(tensor, scale=0.1, zero_point=-3)

        np.testing.assert_array_equal(result[0, 0, 0, :5], [-23, -8, -3, 2, 127])

        tensor[0, 0, 1, :4] = [-0.75, -0.25, 0.25, 0.75]
        tie_result = quantize_model_input(tensor, scale=0.5, zero_point=0)
        np.testing.assert_array_equal(tie_result[0, 0, 1, :4], [-2, -1, 1, 2])

    def test_preprocess_rejects_wrong_contract(self) -> None:
        with self.assertRaises(ValueError):
            letterbox_vga_rgb(np.zeros((640, 640, 3), dtype=np.uint8))
        with self.assertRaises(TypeError):
            letterbox_vga_rgb(np.zeros((480, 640, 3), dtype=np.float32))


class PostprocessTests(unittest.TestCase):
    @staticmethod
    def _raw() -> np.ndarray:
        return np.zeros((1, 5, 8400), dtype=np.float32)

    def test_decode_restores_letterbox_and_clips_vga(self) -> None:
        raw = self._raw()
        raw[0, :, 7] = [100.0, 100.0, 40.0, 60.0, 0.8]
        raw[0, :, 9] = [630.0, 550.0, 40.0, 60.0, 0.7]

        detections, metrics = decode_yolo11_output(raw)

        self.assertEqual([item.candidate_index for item in detections], [7, 9])
        self.assertEqual(detections[0], Detection(7, detections[0].confidence, 80, 0, 120, 50))
        self.assertEqual(detections[1].x2, 640)
        self.assertEqual(detections[1].y2, 480)
        self.assertEqual(metrics["scanned"], 8400)

    def test_decode_rejects_padding_only_and_nonfinite_boxes(self) -> None:
        raw = self._raw()
        raw[0, :, 0] = [100.0, 20.0, 10.0, 10.0, 0.9]
        raw[0, :, 1] = [np.nan, 100.0, 10.0, 10.0, 0.8]
        raw[0, :, 2] = [100.0, 100.0, -10.0, 10.0, 0.7]

        detections, metrics = decode_yolo11_output(raw)

        self.assertEqual(detections, [])
        self.assertEqual(metrics["above_threshold"], 1)
        self.assertEqual(metrics["geometry_rejected"], 2)
        self.assertEqual(metrics["nonfinite_rejected"], 1)
        self.assertEqual(metrics["valid_geometry"], 0)
        self.assertEqual(metrics["pool_overflow"], 0)

    def test_bounded_pool_is_stable_for_score_ties(self) -> None:
        raw = self._raw()
        for candidate_index, confidence in ((4, 0.8), (2, 0.8), (9, 0.9)):
            raw[0, :, candidate_index] = [200.0, 200.0, 20.0, 20.0, confidence]

        detections, metrics = decode_yolo11_output(raw, candidate_capacity=2)

        self.assertEqual([item.candidate_index for item in detections], [9, 2])
        self.assertEqual(metrics["pool_overflow"], 1)

    def test_decoder_uses_exported_confidence_without_extra_sigmoid(self) -> None:
        raw = self._raw()
        raw[0, :, 3] = [100.0, 200.0, 20.0, 20.0, 0.24]
        raw[0, :, 5] = [100.0, 200.0, 20.0, 20.0, 0.25]

        detections, _ = decode_yolo11_output(raw, confidence_threshold=0.25)

        self.assertEqual([item.candidate_index for item in detections], [5])

    def test_global_nms_suppresses_duplicates_after_global_restore(self) -> None:
        candidates = [
            Detection(2, 0.9, 10, 10, 110, 110),
            Detection(3, 0.8, 20, 20, 120, 120),
            Detection(4, 0.7, 300, 300, 350, 350),
        ]

        retained = global_nms(candidates, iou_threshold=0.45)

        self.assertEqual([item.candidate_index for item in retained], [2, 4])

    def test_global_nms_invalidates_result_on_firmware_capacity_overflow(self) -> None:
        candidates = [
            Detection(index, 0.9, index * 10, 0, index * 10 + 4, 4)
            for index in range(3)
        ]

        with self.assertRaisesRegex(
            ResultCapacityError,
            "exceeds firmware capacity of 2 detections",
        ):
            global_nms(candidates, result_capacity=2)

    def test_wrong_output_shape_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            decode_yolo11_output(np.zeros((1, 8400, 5), dtype=np.float32))

    def test_split_outputs_reassemble_with_exact_concat_parity(self) -> None:
        boxes = np.arange(4 * 8400, dtype=np.float32).reshape(1, 4, 8400)
        scores = np.linspace(0.0, 1.0, 8400, dtype=np.float32).reshape(1, 1, 8400)
        expected = np.concatenate([boxes, scores], axis=1)

        actual = assemble_yolo_output([boxes, scores])

        self.assertTrue(np.array_equal(actual, expected))
        self.assertIs(assemble_yolo_output([expected]), expected)

    def test_six_final_conv_outputs_assemble_in_fixed_scale_order(self) -> None:
        outputs = []
        for scale_index, spatial in enumerate((40, 20, 10), start=1):
            candidate_count = spatial * spatial
            regression = np.arange(
                64 * candidate_count,
                dtype=np.float32,
            ).reshape(1, 64, spatial, spatial)
            classification = np.full(
                (1, 1, spatial, spatial),
                np.float32(scale_index),
                dtype=np.float32,
            )
            outputs.extend((regression, classification))

        regression_logits, class_logits = assemble_six_head_logits(outputs)
        expected_regression = np.concatenate(
            [output.reshape(1, 64, -1) for output in outputs[0::2]],
            axis=2,
        )
        expected_class = np.concatenate(
            [output.reshape(1, 1, -1) for output in outputs[1::2]],
            axis=2,
        )

        np.testing.assert_array_equal(regression_logits, expected_regression)
        np.testing.assert_array_equal(class_logits, expected_class)
        self.assertEqual(regression_logits.shape, (1, 64, 2100))
        self.assertEqual(class_logits.shape, (1, 1, 2100))
        np.testing.assert_array_equal(
            assemble_yolo_output(outputs),
            reconstruct_yolov8_output(regression_logits, class_logits),
        )

        malformed = list(outputs)
        malformed[2] = np.zeros((1, 64, 10, 10), dtype=np.float32)
        with self.assertRaisesRegex(ValueError, "regression_s16 shape"):
            assemble_six_head_logits(malformed)
        wrong_dtype = list(outputs)
        wrong_dtype[-1] = wrong_dtype[-1].astype(np.float64)
        with self.assertRaisesRegex(TypeError, "class_s32 dtype"):
            assemble_six_head_logits(wrong_dtype)

    def test_zero_logits_reconstruct_known_dfl_anchor_stride_values(self) -> None:
        regression_logits = np.zeros((1, 64, 8400), dtype=np.float32)
        class_logits = np.zeros((1, 1, 8400), dtype=np.float32)

        anchors, strides = build_yolov8_anchor_grid()
        actual = reconstruct_yolov8_output(regression_logits, class_logits)

        np.testing.assert_array_equal(anchors[0, :, 0], [0.5, 0.5])
        np.testing.assert_array_equal(anchors[0, :, 6399], [79.5, 79.5])
        np.testing.assert_array_equal(anchors[0, :, 6400], [0.5, 0.5])
        np.testing.assert_array_equal(anchors[0, :, 8399], [19.5, 19.5])
        self.assertEqual((strides[0, 0], strides[0, 6400], strides[0, 8000]), (8, 16, 32))
        np.testing.assert_allclose(
            actual[0][:, [0, 6399, 6400, 8399]],
            np.asarray(
                [
                    [4.0, 636.0, 8.0, 624.0],
                    [4.0, 636.0, 8.0, 624.0],
                    [120.0, 120.0, 240.0, 480.0],
                    [120.0, 120.0, 240.0, 480.0],
                    [0.5, 0.5, 0.5, 0.5],
                ],
                dtype=np.float32,
            ),
            rtol=0.0,
            atol=0.0,
        )
        np.testing.assert_array_equal(
            assemble_yolo_output([regression_logits, class_logits]),
            actual,
        )

    def test_full320_zero_logits_and_decode_restore_vga_coordinates(self) -> None:
        regression_logits = np.zeros((1, 64, 2100), dtype=np.float32)
        class_logits = np.zeros((1, 1, 2100), dtype=np.float32)

        actual = reconstruct_yolov8_output(regression_logits, class_logits)

        np.testing.assert_array_equal(actual[0, :, 0], [4, 4, 120, 120, 0.5])
        np.testing.assert_array_equal(actual[0, :, 1600], [8, 8, 240, 240, 0.5])
        raw = np.zeros((1, 5, 2100), dtype=np.float32)
        raw[0, :, 7] = [50.0, 50.0, 20.0, 20.0, 0.9]
        detections, metrics = decode_yolo11_output(raw)
        self.assertEqual(
            detections,
            [Detection(7, detections[0].confidence, 80.0, 0.0, 120.0, 40.0)],
        )
        self.assertEqual(metrics["scanned"], 2100)

    def test_reconstruction_accepts_frozen_qat_dfl_projection(self) -> None:
        regression_logits = np.zeros((1, 64, 2100), dtype=np.float32)
        class_logits = np.zeros((1, 1, 2100), dtype=np.float32)
        projection = np.linspace(0.0, 30.0, 16, dtype=np.float32)

        actual = reconstruct_yolov8_output(
            regression_logits,
            class_logits,
            projection,
        )

        np.testing.assert_array_equal(actual[0, :, 0], [4, 4, 240, 240, 0.5])
        np.testing.assert_array_equal(
            assemble_yolo_output(
                [regression_logits, class_logits],
                projection,
            ),
            actual,
        )
        with self.assertRaisesRegex(TypeError, "DFL projection must be float32"):
            reconstruct_yolov8_output(
                regression_logits,
                class_logits,
                np.arange(16, dtype=np.float64),
            )
        with self.assertRaisesRegex(ValueError, "DFL projection shape"):
            reconstruct_yolov8_output(
                regression_logits,
                class_logits,
                np.arange(15, dtype=np.float32),
            )


class ManifestTests(unittest.TestCase):
    def test_static_model_contract_freezes_smoke_test_identity(self) -> None:
        contract = load_model_contract()
        self.assertEqual(
            contract["source_model"]["sha256"],
            "f0f06bf0032e62f14da5882f87e5440585796f9710de67482775fcb99202270c",
        )
        self.assertEqual(contract["input"]["shape"], [1, 3, 640, 640])
        self.assertEqual(contract["output"]["shape"], [1, 5, 8400])
        self.assertFalse(contract["output"]["extra_sigmoid_required"])
        self.assertEqual(contract["postprocessing"]["result_capacity"], 64)
        self.assertEqual(
            contract["postprocessing"]["result_overflow_policy"],
            "invalidate_result",
        )

    def test_yolov8n_candidate_reuses_existing_contract(self) -> None:
        candidate_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full640_candidate.json"
        )
        candidate = load_model_contract(candidate_path)
        baseline = load_model_contract()

        self.assertEqual(
            candidate["source_model"]["sha256"],
            "3145575023728b64054083061b0afb4288ad47a7014ba9e862106d5e31c09197",
        )
        self.assertEqual(candidate["input"], baseline["input"])
        self.assertEqual(candidate["output"], baseline["output"])
        self.assertEqual(
            candidate["source_preprocessing"], baseline["source_preprocessing"]
        )
        self.assertNotIn("MatMul", candidate["onnx"]["operator_counts"])
        self.assertEqual(candidate["onnx"]["operator_counts"]["Softmax"], 1)
        self.assertEqual(candidate["training_evidence"]["best_epoch"], 126)
        self.assertIsNone(
            candidate["training_evidence"]["dataset"]["split_membership_hash"]
        )
        self.assertEqual(
            candidate["deployment_gate"]["ruhmi_conversion_status"],
            "completed; rejected for single-output INT8 quantization",
        )
        self.assertEqual(
            candidate["deployment_gate"]["ethos_u55_allocation_status"],
            "3 ARM_ETHOS_U55 regions plus 4 C_CODEGEN regions",
        )

    def test_split_output_candidate_freezes_derived_identity(self) -> None:
        contract_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full640_split_outputs.json"
        )
        contract = load_model_contract(contract_path)

        self.assertEqual(
            contract["derivation"]["transform_id"],
            "split-terminal-concat-v1",
        )
        self.assertFalse(contract["derivation"]["network_calculations_changed"])
        self.assertEqual(
            contract["derivation"]["source_model"]["sha256"],
            "3145575023728b64054083061b0afb4288ad47a7014ba9e862106d5e31c09197",
        )
        self.assertEqual(
            contract["source_model"]["sha256"],
            "db640636180eb2f1c34b8a1ad3ffd068ab5c5a8a9586c0f044eed1cf15d36d86",
        )
        self.assertEqual(
            [(output["name"], output["shape"]) for output in contract["outputs"]],
            [("boxes", [1, 4, 8400]), ("scores", [1, 1, 8400])],
        )
        self.assertEqual(
            contract["original_single_output_rejection"]["terminal_score_quantized_max"],
            0.0,
        )
        self.assertEqual(
            contract["deployment_gate"]["ruhmi_conversion_status"],
            "completed; rejected for score quantization",
        )
        self.assertEqual(
            contract["ruhmi_evidence"]["quantization_rejection"]["scores"][
                "quantized_max"
            ],
            0.118,
        )

    def test_predecode_candidate_freezes_derived_identity_and_parity(self) -> None:
        contract_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full640_predecode_outputs.json"
        )
        contract = load_model_contract(contract_path)

        self.assertEqual(
            contract["derivation"]["transform_id"],
            "remove-yolov8-detect-tail-v1",
        )
        self.assertEqual(contract["derivation"]["removed_node_count"], 20)
        self.assertEqual(contract["derivation"]["removed_initializer_count"], 11)
        self.assertEqual(
            contract["source_model"]["sha256"],
            "8a08be195ca64735f4fb5620031799860100dec501fefd83aad48f0e727ffe0a",
        )
        self.assertEqual(
            [(output["name"], output["shape"]) for output in contract["outputs"]],
            [
                ("regression_logits", [1, 64, 8400]),
                ("class_logits", [1, 1, 8400]),
            ],
        )
        self.assertLessEqual(
            contract["host_parity"]["maximum_absolute_difference"],
            contract["host_parity"]["tolerance"]["absolute"],
        )
        self.assertTrue(contract["host_parity"]["retained_indices_equal"])
        self.assertEqual(contract["host_parity"]["retained_count"], 33)

    def test_qat_handoff_freezes_fp32_gate_and_ruhmi_rejection(self) -> None:
        contract_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full320_qat_handoff.json"
        )
        contract = load_model_contract(contract_path)

        self.assertEqual(
            contract["source_model"]["sha256"],
            "4a27181d09b11701af4b83a286ed4e0505a34a467630aecb21848a1958d1d88c",
        )
        self.assertEqual(contract["input"]["shape"], [1, 3, 320, 320])
        self.assertEqual(contract["input"]["dtype"], "float32")
        self.assertFalse(contract["input"]["dynamic_axes"])
        self.assertEqual(
            [(output["name"], output["shape"]) for output in contract["outputs"]],
            [("boxes", [1, 4, 2100]), ("scores", [1, 1, 2100])],
        )
        self.assertFalse(contract["onnx"]["fp16_present"])
        self.assertFalse(contract["onnx"]["embedded_nms"])
        self.assertFalse(contract["onnx"]["split_output_is_combined_tensor"])
        self.assertEqual(contract["onnx"]["initializer_dtypes"], {"FLOAT": 127})
        self.assertEqual(contract["onnx"]["operator_counts"]["Conv"], 64)
        self.assertEqual(contract["onnx"]["operator_counts"]["Softmax"], 1)
        self.assertNotIn("MatMul", contract["onnx"]["operator_counts"])
        self.assertNotIn("NonMaxSuppression", contract["onnx"]["operator_counts"])
        self.assertFalse(contract["derivation"]["network_calculations_changed"])
        self.assertTrue(contract["derivation"]["initializers_byte_identical"])
        self.assertTrue(
            contract["derivation"]["non_packaging_nodes_byte_identical"]
        )

        golden = contract["independent_host_verification"]["fixed_golden"]
        self.assertEqual(
            golden["input_file_sha256"],
            "32d8bf4839fdbb645bdc4bc06300c6d77a6b648698fd24fb936e3c1926c4af3a",
        )
        self.assertTrue(golden["exact_split_parity"])
        self.assertEqual(golden["max_abs_split_difference"], 0.0)
        self.assertGreater(golden["score_max"], 0.25)
        self.assertEqual(golden["candidate_metrics"]["above_threshold"], 245)
        self.assertEqual(golden["nms_detections_at_0_25_iou_0_45"], 32)

        dataset = contract["training_provenance"]["dataset"]
        self.assertEqual(
            dataset["aggregate_sha256"],
            "d2c4d7dac7f2c91a0f3a75358e76c05215be22037a1b40d91a91f7200299a627",
        )
        self.assertEqual(
            dataset["split_counts"], {"train": 2735, "val": 339, "test": 348}
        )
        self.assertTrue(dataset["split_disjoint"])
        self.assertEqual(len(contract["handoff_artifacts"]), 25)
        self.assertEqual(
            contract["training_provenance"]["base_checkpoint"]["sha256"],
            "97beeb3772fd71539d4d62544faa99163ac1e7f3c0efec2cb51c8385813ce4c8",
        )
        self.assertFalse(
            contract["report_audit"]["smoke_report"]["current_artifacts_match"]
        )

        ruhmi = contract["ruhmi_evidence"]
        self.assertEqual(
            ruhmi["compiler"]["log_sha256"],
            "43c405ba110c34b335028086009aee51e7f8a50133490cc31ad78091029299fe",
        )
        self.assertEqual(
            [
                region["npu_operators"]
                for region in ruhmi["operator_allocation"]["vela_regions"]
            ],
            [156, 27, 190],
        )
        self.assertEqual(
            [
                region["cpu_operators"]
                for region in ruhmi["operator_allocation"]["vela_regions"]
            ],
            [0, 0, 0],
        )
        saved_golden = ruhmi["saved_model_fixed_golden"]
        self.assertFalse(saved_golden["accepted_for_firmware_integration"])
        self.assertEqual(saved_golden["score_max"], 0.36786559224128723)
        self.assertEqual(saved_golden["postprocess"]["above_threshold"], 11)
        self.assertEqual(saved_golden["postprocess"]["geometry_rejected"], 11)
        self.assertEqual(saved_golden["postprocess"]["valid_geometry"], 0)
        self.assertEqual(saved_golden["postprocess"]["nms_detections"], 0)
        self.assertEqual(saved_golden["status"], "rejected")
        collapse = ruhmi["quantization_quality"]["critical_collapse"]
        self.assertEqual(collapse["node"], "_model_22_Div_1_output_0_70464")
        self.assertEqual(
            (collapse["fp32_min"], collapse["fp32_max"]), (0.304, 39.658)
        )
        self.assertEqual(
            (collapse["quantized_min"], collapse["quantized_max"]), (0.0, 0.0)
        )
        self.assertEqual(
            ruhmi["root_cause_assessment"]["io_desc_sha256"],
            "10b941a4782eb6804ee792f561d9068082717d7f4b1259f89c5589ded41f7409",
        )
        self.assertIn(
            "QAT activation coverage gap",
            ruhmi["root_cause_assessment"]["classification"],
        )
        self.assertEqual(
            contract["intake"]["status"],
            "rejected_after_single_pinned_ruhmi_gate",
        )
        self.assertEqual(
            contract["deployment_gate"]["ruhmi_conversion_status"],
            "completed once; rejected by saved-model fixed-golden validation",
        )
        self.assertEqual(contract["deployment_gate"]["status"], "rejected")
        self.assertEqual(
            contract["deployment_gate"]["firmware_backend_status"],
            "replacement-model-pending",
        )
        self.assertEqual(
            contract["deployment_gate"]["board_deployment_status"],
            "not attempted",
        )
        recommendation = contract["deployment_gate"]["recommendation"]
        for prohibited_action in (
            "tune the threshold",
            "another compiler variant",
            "substitute CPU deployment",
            "integrate this model into firmware",
        ):
            self.assertIn(prohibited_action, recommendation)

    def test_qat_predecode_manifest_freezes_fp32_pass_and_ruhmi_rejection(self) -> None:
        contract_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full320_qat_predecode_candidate.json"
        )
        contract = load_model_contract(contract_path)

        self.assertEqual(
            contract["immutable_source"]["sha256"],
            "a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2",
        )
        self.assertEqual(
            contract["source_model"]["sha256"],
            "154f55cef66bf2aa9d108e0c1b9110e83299ba987e97275338f4dbdf8103424d",
        )
        self.assertEqual(
            contract["direct_predecode_model"]["sha256"],
            "0a7d09348cc6c6d8343d5c6fb3a9e967bd7b1a23a6eb2246f67ea1913d4a3c5b",
        )
        self.assertEqual(
            [(item["name"], item["shape"]) for item in contract["outputs"]],
            [
                ("regression_logits", [1, 64, 2100]),
                ("class_logits", [1, 1, 2100]),
            ],
        )
        self.assertFalse(contract["onnx"]["dynamic_interface_axes"])
        self.assertFalse(contract["onnx"]["fp16_present"])
        self.assertFalse(contract["onnx"]["embedded_nms"])
        self.assertNotIn("NonMaxSuppression", contract["onnx"]["operator_counts"])
        self.assertEqual(contract["derivation"]["removed_node_count"], 34)
        self.assertEqual(contract["derivation"]["retained_conv_count"], 63)
        self.assertTrue(contract["derivation"]["direct_outputs_produced_by_split"])
        self.assertTrue(
            contract["derivation"]["deterministic_transform_rerun_equal"]
        )
        self.assertEqual(
            contract["derivation"]["dfl_projection_sha256"],
            "59caad87de2582a742029e5c0a6bc3540ca50ff92de2a65aa300c0af3af126db",
        )
        qualification = contract["fp32_qualification"]
        self.assertEqual(qualification["status"], "passed")
        self.assertEqual(qualification["parity_case_count"], 3)
        self.assertEqual(qualification["deterministic_synthetic_count"], 2)
        self.assertTrue(qualification["every_candidate_index_sequence_equal"])
        self.assertTrue(qualification["every_nms_index_sequence_equal"])
        self.assertEqual(qualification["cases"][0]["retained_candidate_count"], 245)
        self.assertEqual(qualification["cases"][0]["nms_count"], 32)
        self.assertLessEqual(
            qualification["cases"][0]["maximum_absolute_difference"],
            qualification["tolerance"]["absolute"],
        )
        historical = contract["historical_frontend_import_risk"]
        self.assertEqual(historical["exit_status"], 3221226505)
        self.assertEqual(historical["exit_status_hex"], "0xC0000409")
        self.assertEqual(len(historical["attempts"]), 2)
        self.assertEqual(
            historical["frontend_preflight_status"],
            "input-description preflight passed but Quantizer frontend import failed",
        )
        current_attempt = historical["current_qat_attempt"]
        self.assertEqual(current_attempt["command_exit_code"], 1)
        self.assertEqual(current_attempt["frontend_exit_status"], 3221226505)
        self.assertTrue(
            current_attempt["optimized_onnx"]["within_frozen_fp32_tolerance"]
        )
        self.assertFalse(current_attempt["saved_model_generated"])
        self.assertEqual(current_attempt["mera_file_count"], 0)
        ruhmi = contract["ruhmi_gate"]
        self.assertEqual(ruhmi["maximum_permitted_quantization_compile_invocations"], 1)
        self.assertEqual(ruhmi["completed_quantization_compile_invocations"], 1)
        self.assertEqual(ruhmi["saved_model_status"], "not generated")
        self.assertEqual(ruhmi["allocation_status"], "not reached")
        self.assertEqual(
            contract["deployment_gate"]["status"],
            "rejected-current-pinned-ruhmi-frontend-crash",
        )
        rejection = contract["prior_decoded_qat_rejection"]
        rejection_path = REPOSITORY_ROOT / rejection["manifest_path"]
        self.assertEqual(sha256_file(rejection_path), rejection["manifest_sha256"])

    def test_qat_six_head_manifest_freezes_saved_model_rejection(self) -> None:
        contract_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolov8n_wheat_full320_qat_six_head_candidate.json"
        )
        contract = load_model_contract(contract_path)

        self.assertEqual(
            contract["immutable_source"]["sha256"],
            "a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2",
        )
        self.assertEqual(
            contract["source_model"]["sha256"],
            "8cdfaa986fb5fe3e841d3c1182ca04075e3fa15a6462be49727b8c83f8ad7e0b",
        )
        self.assertEqual(
            [(item["name"], item["shape"]) for item in contract["outputs"]],
            [
                ("regression_s8", [1, 64, 40, 40]),
                ("class_s8", [1, 1, 40, 40]),
                ("regression_s16", [1, 64, 20, 20]),
                ("class_s16", [1, 1, 20, 20]),
                ("regression_s32", [1, 64, 10, 10]),
                ("class_s32", [1, 1, 10, 10]),
            ],
        )
        self.assertTrue(contract["onnx"]["all_output_producers_are_direct_conv"])
        self.assertFalse(contract["onnx"]["terminal_output_split_present"])
        self.assertFalse(contract["onnx"]["terminal_output_identity_present"])
        self.assertEqual(contract["onnx"]["internal_backbone_split_count"], 8)
        self.assertEqual(contract["derivation"]["removed_node_count"], 46)
        self.assertEqual(
            len(contract["derivation"]["final_conv_artifacts"]),
            6,
        )

        fp32 = contract["fp32_qualification"]
        self.assertEqual(fp32["status"], "passed")
        self.assertTrue(fp32["deterministic_report_rerun_equal"])
        self.assertEqual(
            [(item["retained_candidate_count"], item["nms_count"]) for item in fp32["cases"]],
            [(245, 32), (0, 0), (3, 2)],
        )
        self.assertTrue(
            all(
                item["maximum_absolute_difference"]
                <= fp32["tolerance"]["absolute"]
                for item in fp32["cases"]
            )
        )

        preflight = contract["quantizer_constructor_preflight"]
        self.assertEqual(preflight["status"], "passed")
        for forbidden_count in (
            "calibration_calls",
            "quantization_calls",
            "vela_calls",
            "codegen_calls",
        ):
            self.assertEqual(preflight[forbidden_count], 0)

        ruhmi = contract["ruhmi_gate"]
        self.assertEqual(ruhmi["completed_compile_invocations"], 1)
        self.assertEqual(
            ruhmi["saved_model"]["sha256"],
            "301f97207813c03e12adeacf5c6a285270ee76458a5347c44d12d797764322cd",
        )
        self.assertEqual(
            [item["npu_operators"] for item in ruhmi["vela_regions"]],
            [156, 27, 135],
        )
        self.assertEqual(
            [item["cpu_operators"] for item in ruhmi["vela_regions"]],
            [0, 0, 0],
        )
        self.assertFalse(ruhmi["no_cpu_neural_network_runtime_requirement_met"])
        self.assertEqual(len(ruhmi["cpu_c_codegen_regions"]), 4)
        self.assertIn(
            "UpsamplingNearestNeighbor",
            ruhmi["cpu_c_codegen_regions"][1]["operations"][0],
        )
        self.assertIn(
            "UpsamplingNearestNeighbor",
            ruhmi["cpu_c_codegen_regions"][2]["operations"][0],
        )

        saved = contract["saved_model_fixed_golden"]
        self.assertEqual(saved["status"], "rejected")
        self.assertTrue(saved["all_six_outputs_finite"])
        self.assertTrue(saved["all_six_outputs_noncollapsed"])
        self.assertEqual(saved["fp32_candidate_count"], 245)
        self.assertEqual(saved["saved_candidate_count"], 8)
        self.assertEqual(saved["candidate_index_intersection_count"], 4)
        self.assertEqual(saved["fp32_nms_count"], 32)
        self.assertEqual(saved["saved_nms_count"], 2)
        self.assertEqual(saved["nms_index_intersection_count"], 0)
        self.assertFalse(saved["accepted_for_firmware_integration"])

        deployment = contract["deployment_gate"]
        self.assertEqual(
            deployment["status"],
            "rejected-after-single-pinned-ruhmi-gate",
        )
        self.assertEqual(
            deployment["firmware_backend_status"],
            "replacement-model-pending",
        )
        self.assertEqual(deployment["board_deployment_status"], "not attempted")
        self.assertIn("firmware integration", deployment["prohibited_recovery_actions"])

        generated_artifacts = (
            contract["source_model"],
            {
                "generated_path": fp32["evidence_path"],
                "bytes": fp32["evidence_bytes"],
                "sha256": fp32["evidence_sha256"],
            },
            {
                "generated_path": preflight["log_path"],
                "bytes": preflight["log_bytes"],
                "sha256": preflight["log_sha256"],
            },
            {
                "generated_path": ruhmi["compiler_log_path"],
                "bytes": ruhmi["compiler_log_bytes"],
                "sha256": ruhmi["compiler_log_sha256"],
            },
            ruhmi["saved_model"],
            {
                "generated_path": ruhmi["io_desc_path"],
                "bytes": ruhmi["io_desc_bytes"],
                "sha256": ruhmi["io_desc_sha256"],
            },
            {
                "generated_path": saved["report_path"],
                "bytes": saved["report_bytes"],
                "sha256": saved["report_sha256"],
            },
        )
        for artifact in generated_artifacts:
            path = REPOSITORY_ROOT / artifact["generated_path"]
            if path.is_file():
                self.assertEqual(path.stat().st_size, artifact["bytes"])
                self.assertEqual(sha256_file(path), artifact["sha256"])

    def test_manifest_is_deterministic_and_detects_source_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            dataset_root = Path(temporary_directory)
            self._make_fake_dataset(dataset_root)

            first = build_gwhd_manifest(dataset_root)
            second = build_gwhd_manifest(dataset_root)

            self.assertEqual(first, second)
            self.assertEqual(first["selection"]["calibration_image_count"], 35)
            self.assertEqual(first["selection"]["golden_image_count"], 21)
            self.assertEqual(verify_gwhd_manifest(dataset_root, first)["images"], 35)

            changed_path = dataset_root / first["images"][0]["relative_path"]
            changed_path.write_bytes(b"changed")
            with self.assertRaises(ValueError):
                verify_gwhd_manifest(dataset_root, first)

    def test_square_gwhd_image_becomes_vga_rgb(self) -> None:
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        with tempfile.TemporaryDirectory() as temporary_directory:
            source_path = Path(temporary_directory) / "source.png"
            pixels = np.zeros((8, 8, 3), dtype=np.uint8)
            pixels[:, :, 0] = np.arange(8, dtype=np.uint8)[None, :]
            Image.fromarray(pixels, "RGB").save(source_path)

            result = load_gwhd_vga_rgb(source_path)

            self.assertEqual(result.shape, (480, 640, 3))
            self.assertEqual(result.dtype, np.uint8)

    @staticmethod
    def _make_fake_dataset(root: Path) -> None:
        for domain_index in range(7):
            domain = f"domain_{domain_index}"
            domain_root = root / domain
            domain_root.mkdir()
            images = []
            annotations = []
            annotation_id = 1
            for image_index in range(5):
                file_name = f"image_{image_index}.png"
                relative_path = f"{domain}/{file_name}"
                (domain_root / file_name).write_bytes(
                    f"{domain}:{image_index}".encode("ascii")
                )
                image_id = image_index + 1
                images.append(
                    {
                        "id": image_id,
                        "path": relative_path,
                        "file_name": file_name,
                        "width": 1024,
                        "height": 1024,
                    }
                )
                for _ in range(image_index):
                    annotations.append(
                        {
                            "id": annotation_id,
                            "image_id": image_id,
                            "category_id": 1,
                            "bbox": [1, 1, 10, 10],
                        }
                    )
                    annotation_id += 1
            payload = {
                "categories": [{"id": 1, "name": "wheat_head"}],
                "images": images,
                "annotations": annotations,
            }
            (root / f"{domain}.json").write_text(
                json.dumps(payload, sort_keys=True), encoding="utf-8"
            )


class ModelTransformTests(unittest.TestCase):
    def test_real_qat_six_head_qualification_when_assets_exist(self) -> None:
        source_path = (
            REPOSITORY_ROOT
            / "models"
            / "QAT_YOLOv8n_320_handoff_20260809"
            / "model_and_reports"
            / "model_combined_fp32.onnx"
        )
        golden_path = (
            REPOSITORY_ROOT
            / "output"
            / "ruhmi_yolov8n_320"
            / "golden_fp32_00"
            / "input_fp32.npy"
        )
        if not source_path.is_file() or not golden_path.is_file():
            self.skipTest("QAT source or fixed golden is not present")
        try:
            import onnx
            import onnxruntime  # noqa: F401
        except ImportError:
            self.skipTest("onnx or onnxruntime is not installed")

        with tempfile.TemporaryDirectory() as temporary_directory:
            output_dir = Path(temporary_directory) / "qualification"
            report = qualify_six_head_candidate(
                source_path,
                golden_path,
                output_dir,
                expected_source_sha256=(
                    "a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2"
                ),
            )
            model = onnx.load(
                str(
                    output_dir
                    / "derived"
                    / "yolov8n_wheat_full320_qat_six_head_outputs.onnx"
                ),
                load_external_data=False,
            )

        self.assertEqual(report["status"], "fp32_static_qualified")
        self.assertEqual(
            report["artifacts"]["six_head"]["sha256"],
            "8cdfaa986fb5fe3e841d3c1182ca04075e3fa15a6462be49727b8c83f8ad7e0b",
        )
        self.assertEqual(len(report["transform"]["removed_node_names"]), 46)
        self.assertTrue(report["transform"]["deterministic_rerun_equal"])
        self.assertEqual(
            [output.name for output in model.graph.output],
            [
                "regression_s8",
                "class_s8",
                "regression_s16",
                "class_s16",
                "regression_s32",
                "class_s32",
            ],
        )
        producers = {
            output.name: [
                node.op_type for node in model.graph.node if output.name in node.output
            ]
            for output in model.graph.output
        }
        self.assertTrue(all(value == ["Conv"] for value in producers.values()))
        self.assertEqual(report["cases"][0]["retained_candidate_count"], 245)
        self.assertEqual(report["cases"][0]["nms_count"], 32)
        self.assertLessEqual(
            report["cases"][0]["maximum_absolute_difference"],
            report["tolerance"]["absolute"],
        )
        self.assertEqual(
            report["ruhmi"]["quantizer_constructor_preflight_status"],
            "pending_parent_owned",
        )

    def test_real_six_head_transform_rejects_source_drift_when_assets_exist(
        self,
    ) -> None:
        source_path = (
            REPOSITORY_ROOT
            / "models"
            / "QAT_YOLOv8n_320_handoff_20260809"
            / "model_and_reports"
            / "model_combined_fp32.onnx"
        )
        if not source_path.is_file():
            self.skipTest("QAT source is not present")
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx is not installed")
        from wheat_deployment.head_transform import (  # noqa: E402
            derive_six_head_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)

            topology_model = onnx.load(str(source_path), load_external_data=False)
            topology_node = next(
                node
                for node in topology_model.graph.node
                if node.name == "/model.22/cv2.0/cv2.0.2/Conv"
            )
            topology_node.name = "/model.22/cv2.0/cv2.0.2/Conv_drift"
            topology_path = root / "topology-drift.onnx"
            onnx.save(topology_model, str(topology_path))
            with self.assertRaisesRegex(ValueError, "expected one final Conv"):
                derive_six_head_output_model(
                    topology_path,
                    root / "topology-derived.onnx",
                )

            weight_model = onnx.load(str(source_path), load_external_data=False)
            initializer = next(
                item
                for item in weight_model.graph.initializer
                if item.name == "model.22.cv2.0.2.weight"
            )
            values = onnx.numpy_helper.to_array(initializer).copy()
            values.flat[0] += np.float32(1.0)
            initializer.CopyFrom(
                onnx.numpy_helper.from_array(values, name=initializer.name)
            )
            weight_path = root / "weight-drift.onnx"
            onnx.save(weight_model, str(weight_path))
            with self.assertRaisesRegex(ValueError, "weight mismatch"):
                derive_six_head_output_model(
                    weight_path,
                    root / "weight-derived.onnx",
                )

    def test_real_qat_opset13_predecode_qualification_when_assets_exist(self) -> None:
        source_path = (
            REPOSITORY_ROOT
            / "models"
            / "QAT_YOLOv8n_320_handoff_20260809"
            / "model_and_reports"
            / "model_combined_fp32.onnx"
        )
        golden_path = (
            REPOSITORY_ROOT
            / "output"
            / "ruhmi_yolov8n_320"
            / "golden_fp32_00"
            / "input_fp32.npy"
        )
        if not source_path.is_file() or not golden_path.is_file():
            self.skipTest("QAT source or fixed golden is not present")
        try:
            import onnx  # noqa: F401
            import onnxruntime  # noqa: F401
        except ImportError:
            self.skipTest("onnx or onnxruntime is not installed")

        with tempfile.TemporaryDirectory() as temporary_directory:
            output_dir = Path(temporary_directory) / "qualification"
            report = qualify_predecode_candidate(
                source_path,
                golden_path,
                output_dir,
                expected_source_sha256=(
                    "a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2"
                ),
            )
            contract_path = (
                TOOLS_ROOT
                / "wheat_deployment"
                / "manifests"
                / "yolov8n_wheat_full320_qat_predecode_candidate.json"
            )
            observed = verify_model_contract(
                output_dir
                / "derived"
                / "yolov8n_wheat_full320_qat_predecode_identity_outputs.onnx",
                contract_path,
            )

        self.assertEqual(report["status"], "fp32_static_qualified")
        self.assertEqual(len(report["transform"]["removed_node_names"]), 34)
        self.assertEqual(
            report["transform"]["split_encoding"], "opset13-static-input"
        )
        self.assertTrue(report["parity"]["every_candidate_index_sequence_equal"])
        self.assertTrue(report["parity"]["every_nms_index_sequence_equal"])
        self.assertFalse(observed["dynamic_interface_axes"])
        self.assertFalse(observed["fp16_present"])
        self.assertFalse(observed["embedded_nms"])

    def test_terminal_concat_transform_and_multi_output_contract(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            TRANSFORM_ID,
            derive_split_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source.onnx"
            derived_path = root / "derived.onnx"
            self._write_synthetic_model(source_path)
            source_sha256 = sha256_file(source_path)

            first = derive_split_output_model(source_path, derived_path)
            second = derive_split_output_model(source_path, derived_path)

            self.assertEqual(first, second)
            self.assertEqual(first["transform_id"], TRANSFORM_ID)
            self.assertEqual(sha256_file(source_path), source_sha256)
            model = onnx.load(str(derived_path), load_external_data=False)
            self.assertEqual(
                [node.op_type for node in model.graph.node],
                ["Identity", "Identity"],
            )
            self.assertEqual(
                [output.name for output in model.graph.output],
                ["boxes", "scores"],
            )
            metadata = {item.key: item.value for item in model.metadata_props}
            self.assertEqual(metadata["renesas_cup.transform"], TRANSFORM_ID)
            self.assertEqual(
                metadata["renesas_cup.transform.source_sha256"],
                source_sha256,
            )

            contract = {
                "source_model": {
                    "bytes": derived_path.stat().st_size,
                    "sha256": sha256_file(derived_path),
                },
                "input": {
                    "name": "images",
                    "dtype": "float32",
                    "shape": [1, 3, 640, 640],
                },
                "outputs": [
                    {
                        "name": "boxes",
                        "dtype": "float32",
                        "shape": [1, 4, 8400],
                        "embedded_nms": False,
                    },
                    {
                        "name": "scores",
                        "dtype": "float32",
                        "shape": [1, 1, 8400],
                        "embedded_nms": False,
                    },
                ],
                "onnx": {
                    "opsets": {"ai.onnx": 12},
                    "ir_version": 7,
                    "producer_name": "synthetic-test",
                    "producer_version": "1",
                    "operator_counts": {"Identity": 2},
                },
            }
            contract_path = root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            observed = verify_model_contract(derived_path, contract_path)
            self.assertEqual(
                [output["shape"] for output in observed["outputs"]],
                [[1, 4, 8400], [1, 1, 8400]],
            )

    def test_terminal_concat_transform_supports_full320_contract(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_split_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source320.onnx"
            derived_path = root / "derived320.onnx"
            onnx.save(
                self._synthetic_model(model_size=320, candidate_count=2100),
                str(source_path),
            )

            result = derive_split_output_model(source_path, derived_path)
            model = onnx.load(str(derived_path), load_external_data=False)

            self.assertEqual(result["model_size"], 320)
            self.assertEqual(result["candidate_count"], 2100)
            self.assertEqual(
                [output["shape"] for output in result["outputs"]],
                [[1, 4, 2100], [1, 1, 2100]],
            )
            self.assertEqual(
                [
                    [dimension.dim_value for dimension in output.type.tensor_type.shape.dim]
                    for output in model.graph.output
                ],
                [[1, 4, 2100], [1, 1, 2100]],
            )

    def test_terminal_concat_transform_rejects_mixed_size_contract(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_split_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "mixed.onnx"
            onnx.save(
                self._synthetic_model(model_size=320, candidate_count=8400),
                str(source_path),
            )

            with self.assertRaisesRegex(
                ValueError,
                "unsupported split-output source contract",
            ):
                derive_split_output_model(source_path, root / "derived.onnx")

    def test_transform_rejects_shared_terminal_concat_input(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_split_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "malformed.onnx"
            derived_path = root / "derived.onnx"
            model = self._synthetic_model()
            model.graph.node.insert(
                2,
                onnx.helper.make_node(
                    "Identity",
                    ["boxes_internal"],
                    ["unexpected_consumer"],
                    name="unexpected-consumer",
                ),
            )
            onnx.save(model, str(source_path))

            with self.assertRaisesRegex(ValueError, "feed only the final Concat"):
                derive_split_output_model(source_path, derived_path)

    def test_predecode_transform_removes_exact_tail_and_freezes_contract(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            PREDECODE_TRANSFORM_ID,
            derive_predecode_output_model,
            wrap_predecode_output_identities,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source.onnx"
            derived_path = root / "derived.onnx"
            onnx.save(self._synthetic_predecode_model(), str(source_path))
            source_sha256 = sha256_file(source_path)

            first = derive_predecode_output_model(source_path, derived_path)
            second = derive_predecode_output_model(source_path, derived_path)

            self.assertEqual(first, second)
            self.assertEqual(first["transform_id"], PREDECODE_TRANSFORM_ID)
            self.assertEqual(len(first["removed_node_names"]), 20)
            self.assertEqual(len(first["removed_initializer_names"]), 11)
            self.assertEqual(sha256_file(source_path), source_sha256)
            model = onnx.load(str(derived_path), load_external_data=False)
            self.assertEqual(
                [(node.name, node.op_type) for node in model.graph.node],
                [("feature-producer", "Identity"), ("/model.22/Split", "Split")],
            )
            self.assertEqual(
                [output.name for output in model.graph.output],
                ["regression_logits", "class_logits"],
            )
            self.assertEqual(
                [initializer.name for initializer in model.graph.initializer],
                ["feature_values"],
            )
            metadata = {item.key: item.value for item in model.metadata_props}
            self.assertEqual(
                metadata["renesas_cup.transform"], PREDECODE_TRANSFORM_ID
            )
            self.assertEqual(
                metadata["renesas_cup.transform.source_sha256"], source_sha256
            )

            contract = {
                "source_model": {
                    "bytes": derived_path.stat().st_size,
                    "sha256": sha256_file(derived_path),
                },
                "input": {
                    "name": "images",
                    "dtype": "float32",
                    "shape": [1, 3, 640, 640],
                },
                "outputs": [
                    {
                        "name": "regression_logits",
                        "dtype": "float32",
                        "shape": [1, 64, 8400],
                        "embedded_nms": False,
                    },
                    {
                        "name": "class_logits",
                        "dtype": "float32",
                        "shape": [1, 1, 8400],
                        "embedded_nms": False,
                    },
                ],
                "onnx": {
                    "opsets": {"ai.onnx": 12},
                    "ir_version": 7,
                    "producer_name": "synthetic-test",
                    "producer_version": "1",
                    "operator_counts": {"Identity": 1, "Split": 1},
                },
            }
            contract_path = root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            observed = verify_model_contract(derived_path, contract_path)
            self.assertEqual(
                [output["shape"] for output in observed["outputs"]],
                [[1, 64, 8400], [1, 1, 8400]],
            )

            wrapped_path = root / "wrapped.onnx"
            wrapped = wrap_predecode_output_identities(derived_path, wrapped_path)
            wrapped_model = onnx.load(str(wrapped_path), load_external_data=False)
            self.assertEqual(wrapped["candidate_count"], 8400)
            self.assertEqual(
                [node.op_type for node in wrapped_model.graph.node[-2:]],
                ["Identity", "Identity"],
            )
            self.assertEqual(
                [list(node.output) for node in wrapped_model.graph.node[-2:]],
                [["regression_logits"], ["class_logits"]],
            )

    def test_predecode_transform_rejects_wrong_softmax_axis(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_predecode_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "malformed.onnx"
            model = self._synthetic_predecode_model()
            softmax = next(
                node for node in model.graph.node if node.name == "/model.22/dfl/Softmax"
            )
            softmax.attribute[0].i = 2
            onnx.save(model, str(source_path))

            with self.assertRaisesRegex(ValueError, "attributes mismatch"):
                derive_predecode_output_model(source_path, root / "derived.onnx")

    def test_predecode_transform_rejects_wrong_split_shape(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_predecode_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "malformed.onnx"
            model = self._synthetic_predecode_model()
            split = next(
                node for node in model.graph.node if node.name == "/model.22/Split"
            )
            split_attribute = next(
                attribute for attribute in split.attribute if attribute.name == "split"
            )
            del split_attribute.ints[:]
            split_attribute.ints.extend([63, 2])
            onnx.save(model, str(source_path))

            with self.assertRaises(ValueError):
                derive_predecode_output_model(source_path, root / "derived.onnx")

    def test_predecode_transform_rejects_source_tensor_fanout(self) -> None:
        try:
            import onnx
        except ImportError:
            self.skipTest("onnx not installed")
        from wheat_deployment.model_transform import (  # noqa: E402
            derive_predecode_output_model,
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "malformed.onnx"
            model = self._synthetic_predecode_model()
            split_index = next(
                index
                for index, node in enumerate(model.graph.node)
                if node.name == "/model.22/Split"
            )
            model.graph.node.insert(
                split_index + 1,
                onnx.helper.make_node(
                    "Identity",
                    ["/model.22/Split_output_0"],
                    ["unexpected_consumer"],
                    name="unexpected-consumer",
                ),
            )
            onnx.save(model, str(source_path))

            with self.assertRaisesRegex(ValueError, "consumers mismatch"):
                derive_predecode_output_model(source_path, root / "derived.onnx")

    @classmethod
    def _write_synthetic_model(cls, path: Path) -> None:
        import onnx

        onnx.save(cls._synthetic_model(), str(path))

    @staticmethod
    def _synthetic_model(*, model_size: int = 640, candidate_count: int = 8400):
        import onnx

        boxes_values = onnx.numpy_helper.from_array(
            np.zeros((1, 4, candidate_count), dtype=np.float32),
            name="boxes_values",
        )
        scores_values = onnx.numpy_helper.from_array(
            np.zeros((1, 1, candidate_count), dtype=np.float32),
            name="scores_values",
        )
        graph = onnx.helper.make_graph(
            [
                onnx.helper.make_node(
                    "Identity",
                    ["boxes_values"],
                    ["boxes_internal"],
                    name="boxes-producer",
                ),
                onnx.helper.make_node(
                    "Identity",
                    ["scores_values"],
                    ["scores_internal"],
                    name="scores-producer",
                ),
                onnx.helper.make_node(
                    "Concat",
                    ["boxes_internal", "scores_internal"],
                    ["output0"],
                    name="terminal-concat",
                    axis=1,
                ),
            ],
            "synthetic-split-output-model",
            [
                onnx.helper.make_tensor_value_info(
                    "images",
                    onnx.TensorProto.FLOAT,
                    [1, 3, model_size, model_size],
                )
            ],
            [
                onnx.helper.make_tensor_value_info(
                    "output0",
                    onnx.TensorProto.FLOAT,
                    [1, 5, candidate_count],
                )
            ],
            [boxes_values, scores_values],
        )
        model = onnx.helper.make_model(
            graph,
            producer_name="synthetic-test",
            producer_version="1",
            opset_imports=[onnx.helper.make_opsetid("", 12)],
        )
        model.ir_version = 7
        return model

    @staticmethod
    def _synthetic_predecode_model():
        import onnx

        anchors, strides = build_yolov8_anchor_grid()
        initializers = [
            onnx.numpy_helper.from_array(
                np.zeros((1, 65, 8400), dtype=np.float32),
                name="feature_values",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([4], dtype=np.int64),
                name="/model.22/Gather_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([1], dtype=np.int64),
                name="/model.22/Constant_5_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([2], dtype=np.int64),
                name="/model.22/Constant_6_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([1, 4, 16, 8400], dtype=np.int64),
                name="/model.22/dfl/Constant_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.arange(16, dtype=np.float32).reshape(1, 16, 1, 1),
                name="model.22.dfl.conv.weight",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([1, 4, 8400], dtype=np.int64),
                name="/model.22/dfl/Constant_1_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.asarray([0], dtype=np.int64),
                name="/model.22/Constant_4_output_0",
            ),
            onnx.numpy_helper.from_array(
                anchors,
                name="/model.22/Constant_9_output_0",
            ),
            onnx.numpy_helper.from_array(
                anchors,
                name="/model.22/Constant_10_output_0",
            ),
            onnx.numpy_helper.from_array(
                np.asarray(2.0, dtype=np.float32),
                name="/model.22/Constant_11_output_0",
            ),
            onnx.numpy_helper.from_array(
                strides,
                name="/model.22/Constant_12_output_0",
            ),
        ]
        nodes = [
            onnx.helper.make_node(
                "Identity",
                ["feature_values"],
                ["/model.22/Concat_3_output_0"],
                name="feature-producer",
            ),
            onnx.helper.make_node(
                "Add",
                ["/model.22/Gather_output_0", "/model.22/Constant_5_output_0"],
                ["/model.22/Add_output_0"],
                name="/model.22/Add",
            ),
            onnx.helper.make_node(
                "Div",
                ["/model.22/Add_output_0", "/model.22/Constant_6_output_0"],
                ["/model.22/Mul_output_0"],
                name="/model.22/Div",
            ),
            onnx.helper.make_node(
                "Mul",
                ["/model.22/Mul_output_0", "/model.22/Constant_6_output_0"],
                ["/model.22/Mul_1_output_0"],
                name="/model.22/Mul_1",
            ),
            onnx.helper.make_node(
                "Split",
                ["/model.22/Concat_3_output_0"],
                list(("/model.22/Split_output_0", "/model.22/Split_output_1")),
                name="/model.22/Split",
                axis=1,
                split=[64, 1],
            ),
            onnx.helper.make_node(
                "Reshape",
                [
                    "/model.22/Split_output_0",
                    "/model.22/dfl/Constant_output_0",
                ],
                ["/model.22/dfl/Reshape_output_0"],
                name="/model.22/dfl/Reshape",
            ),
            onnx.helper.make_node(
                "Sigmoid",
                ["/model.22/Split_output_1"],
                ["/model.22/Sigmoid_output_0"],
                name="/model.22/Sigmoid",
            ),
            onnx.helper.make_node(
                "Transpose",
                ["/model.22/dfl/Reshape_output_0"],
                ["/model.22/dfl/Transpose_output_0"],
                name="/model.22/dfl/Transpose",
                perm=[0, 3, 1, 2],
            ),
            onnx.helper.make_node(
                "Softmax",
                ["/model.22/dfl/Transpose_output_0"],
                ["/model.22/dfl/Softmax_output_0"],
                name="/model.22/dfl/Softmax",
                axis=3,
            ),
            onnx.helper.make_node(
                "Transpose",
                ["/model.22/dfl/Softmax_output_0"],
                ["/model.22/dfl/Transpose_1_output_0"],
                name="/model.22/dfl/Transpose_1",
                perm=[0, 3, 2, 1],
            ),
            onnx.helper.make_node(
                "Conv",
                [
                    "/model.22/dfl/Transpose_1_output_0",
                    "model.22.dfl.conv.weight",
                ],
                ["/model.22/dfl/conv/Conv_output_0"],
                name="/model.22/dfl/conv/Conv",
                dilations=[1, 1],
                group=1,
                kernel_shape=[1, 1],
                pads=[0, 0, 0, 0],
                strides=[1, 1],
            ),
            onnx.helper.make_node(
                "Reshape",
                [
                    "/model.22/dfl/conv/Conv_output_0",
                    "/model.22/dfl/Constant_1_output_0",
                ],
                ["/model.22/dfl/Reshape_1_output_0"],
                name="/model.22/dfl/Reshape_1",
            ),
            onnx.helper.make_node(
                "Slice",
                [
                    "/model.22/dfl/Reshape_1_output_0",
                    "/model.22/Constant_4_output_0",
                    "/model.22/Mul_output_0",
                    "/model.22/Constant_5_output_0",
                ],
                ["/model.22/Slice_output_0"],
                name="/model.22/Slice",
            ),
            onnx.helper.make_node(
                "Slice",
                [
                    "/model.22/dfl/Reshape_1_output_0",
                    "/model.22/Mul_output_0",
                    "/model.22/Mul_1_output_0",
                    "/model.22/Constant_5_output_0",
                ],
                ["/model.22/Slice_1_output_0"],
                name="/model.22/Slice_1",
            ),
            onnx.helper.make_node(
                "Sub",
                ["/model.22/Constant_9_output_0", "/model.22/Slice_output_0"],
                ["/model.22/Sub_output_0"],
                name="/model.22/Sub",
            ),
            onnx.helper.make_node(
                "Add",
                ["/model.22/Constant_10_output_0", "/model.22/Slice_1_output_0"],
                ["/model.22/Add_1_output_0"],
                name="/model.22/Add_1",
            ),
            onnx.helper.make_node(
                "Add",
                ["/model.22/Sub_output_0", "/model.22/Add_1_output_0"],
                ["/model.22/Add_2_output_0"],
                name="/model.22/Add_2",
            ),
            onnx.helper.make_node(
                "Sub",
                ["/model.22/Add_1_output_0", "/model.22/Sub_output_0"],
                ["/model.22/Sub_1_output_0"],
                name="/model.22/Sub_1",
            ),
            onnx.helper.make_node(
                "Div",
                ["/model.22/Add_2_output_0", "/model.22/Constant_11_output_0"],
                ["/model.22/Div_1_output_0"],
                name="/model.22/Div_1",
            ),
            onnx.helper.make_node(
                "Concat",
                ["/model.22/Div_1_output_0", "/model.22/Sub_1_output_0"],
                ["/model.22/Concat_4_output_0"],
                name="/model.22/Concat_4",
                axis=1,
            ),
            onnx.helper.make_node(
                "Mul",
                ["/model.22/Concat_4_output_0", "/model.22/Constant_12_output_0"],
                ["/model.22/Mul_2_output_0"],
                name="/model.22/Mul_2",
            ),
            onnx.helper.make_node(
                "Concat",
                ["/model.22/Mul_2_output_0", "/model.22/Sigmoid_output_0"],
                ["output0"],
                name="/model.22/Concat_5",
                axis=1,
            ),
        ]
        graph = onnx.helper.make_graph(
            nodes,
            "synthetic-predecode-model",
            [
                onnx.helper.make_tensor_value_info(
                    "images",
                    onnx.TensorProto.FLOAT,
                    [1, 3, 640, 640],
                )
            ],
            [
                onnx.helper.make_tensor_value_info(
                    "output0",
                    onnx.TensorProto.FLOAT,
                    [1, 5, 8400],
                )
            ],
            initializers,
        )
        model = onnx.helper.make_model(
            graph,
            producer_name="synthetic-test",
            producer_version="1",
            opset_imports=[onnx.helper.make_opsetid("", 12)],
        )
        model.ir_version = 7
        return model


if __name__ == "__main__":
    unittest.main(verbosity=2)
