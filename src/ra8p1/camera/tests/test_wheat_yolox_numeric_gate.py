from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

import numpy as np


TOOLS_ROOT = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))

from wheat_deployment.yolox_numeric_gate import (  # noqa: E402
    FOCUS_OFFSETS,
    build_focus_inputs,
    compare_int8_outputs,
    summarize_int8,
)


class YoloxNumericGateTests(unittest.TestCase):
    def test_focus_inputs_match_generated_strided_slice_order(self) -> None:
        values = np.arange(1 * 224 * 224 * 3, dtype=np.int32)
        source = ((values % 256) - 128).astype(np.int8).reshape(1, 224, 224, 3)

        focus = build_focus_inputs(source)

        self.assertEqual(set(focus), set(FOCUS_OFFSETS))
        for name, (row, column) in FOCUS_OFFSETS.items():
            np.testing.assert_array_equal(
                focus[name], source[:, row::2, column::2, :]
            )
            self.assertTrue(focus[name].flags.c_contiguous)

    def test_summary_uses_standard_ieee_crc32(self) -> None:
        values = np.frombuffer(b"123456789", dtype=np.int8)

        summary = summarize_int8(values)

        self.assertEqual(summary["crc32"], "0xCBF43926")
        self.assertEqual(summary["bytes"], 9)
        self.assertEqual(
            summary["sha256"], hashlib.sha256(b"123456789").hexdigest()
        )
        self.assertEqual(zlib.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)

    def test_exact_comparison_reports_changed_int8_values(self) -> None:
        expected = np.zeros((1, 1029, 6), dtype=np.int8)
        actual = expected.copy()
        actual[0, 7, 4] = 3

        comparison = compare_int8_outputs(actual, expected)

        self.assertFalse(comparison["exact"])
        self.assertEqual(comparison["changed_values"], 1)
        self.assertEqual(comparison["maximum_absolute_error"], 3)

    def test_focus_builder_rejects_wrong_layout(self) -> None:
        with self.assertRaisesRegex(ValueError, "int8 NHWC"):
            build_focus_inputs(np.zeros((1, 3, 224, 224), dtype=np.int8))

    def test_hash_fixture_is_binary_stable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "output.bin"
            path.write_bytes(bytes(range(32)))
            expected = hashlib.sha256(bytes(range(32))).hexdigest()
            observed = hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual(observed, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
