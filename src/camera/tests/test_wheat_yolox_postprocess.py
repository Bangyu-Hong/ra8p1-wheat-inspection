from __future__ import annotations

from pathlib import Path
import json
import sys
import unittest

import numpy as np


TOOLS_ROOT = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))

from wheat_deployment.yolox_postprocess import (  # noqa: E402
    decode_yolox_raw,
    yolox_class_agnostic_nms,
)


class YoloxPostprocessTests(unittest.TestCase):
    def test_decode_uses_grid_stride_exp_and_score_product(self) -> None:
        raw = np.zeros((1, 1029, 6), dtype=np.float32)
        raw[..., 4:] = 0.1
        raw[0, 0] = [0.5, 0.25, np.log(2.0), np.log(3.0), 0.8, 0.5]
        detections = decode_yolox_raw(raw, confidence_threshold=0.25)

        self.assertEqual(len(detections), 1)
        detection = detections[0]
        self.assertEqual(detection.candidate_index, 0)
        self.assertAlmostEqual(detection.confidence, 0.4, places=6)
        self.assertAlmostEqual(detection.x1, -4.0, places=5)
        self.assertAlmostEqual(detection.y1, -10.0, places=5)
        self.assertAlmostEqual(detection.x2, 12.0, places=5)
        self.assertAlmostEqual(detection.y2, 14.0, places=5)

    def test_nms_is_class_agnostic_and_stable(self) -> None:
        raw = np.zeros((1, 1029, 6), dtype=np.float32)
        raw[..., 4:] = 0.1
        raw[0, 0] = [0.5, 0.5, 0.0, 0.0, 0.9, 0.9]
        raw[0, 1] = [-0.5, 0.5, 0.0, 0.0, 0.8, 0.8]
        raw[0, 20] = [0.5, 0.5, 0.0, 0.0, 0.7, 0.7]

        candidates = decode_yolox_raw(raw, confidence_threshold=0.25)
        retained = yolox_class_agnostic_nms(candidates, iou_threshold=0.45)

        self.assertEqual([item.candidate_index for item in retained], [0, 20])

    def test_probability_channels_reject_negative_and_saturate_overshoot(self) -> None:
        raw = np.zeros((1, 1029, 6), dtype=np.float32)
        raw[..., 4:] = 0.1
        raw[0, 0] = [0.5, 0.5, 0.0, 0.0, -0.8, -0.8]
        raw[0, 1] = [0.5, 0.5, 0.0, 0.0, 1.02, 1.03]

        detections = decode_yolox_raw(raw, confidence_threshold=0.25)

        self.assertEqual([item.candidate_index for item in detections], [1])
        self.assertEqual(detections[0].confidence, 1.0)


class YoloxCandidateManifestTests(unittest.TestCase):
    def test_full224_candidate_remains_rejected_and_nonfinal(self) -> None:
        manifest_path = (
            TOOLS_ROOT
            / "wheat_deployment"
            / "manifests"
            / "yolox_tiny_gwhd_full224_candidate.json"
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(manifest["status"], "rejected")
        self.assertEqual(manifest["purpose"], "full224-chain-smoke")
        self.assertFalse(manifest["scope"]["final_tile_model"])
        self.assertTrue(
            manifest["decision"]["deployment_chain_smoke_completed"]
        )
        self.assertFalse(manifest["decision"]["saved_model_accuracy_accepted"])
        self.assertFalse(manifest["decision"]["allocation_accepted"])
        self.assertFalse(manifest["decision"]["firmware_integration_allowed"])
        self.assertLess(
            manifest["saved_model_gate"]["nms_index_recall"],
            0.25,
        )
        focus_region = manifest["compiler"]["allocation"]["cpu_regions"][0]
        self.assertIn("Focus", focus_region)
        self.assertIn("strided slices", focus_region)


if __name__ == "__main__":
    unittest.main(verbosity=2)
