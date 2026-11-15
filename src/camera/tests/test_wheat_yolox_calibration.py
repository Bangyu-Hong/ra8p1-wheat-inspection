from __future__ import annotations

import csv
from pathlib import Path
import random
import sys
import tempfile
import unittest


TOOLS_ROOT = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))

from wheat_deployment.yolox_calibration import (  # noqa: E402
    CalibrationRow,
    load_split_manifest,
    select_calibration_rows,
)


class YoloxCalibrationSelectionTests(unittest.TestCase):
    @staticmethod
    def _rows() -> list[CalibrationRow]:
        rows: list[CalibrationRow] = []
        for domain_index in range(7):
            domain = f"domain_{domain_index}"
            for item_index in range(50):
                rows.append(
                    CalibrationRow(
                        split="train",
                        source_member=f"{domain}/{item_index:03d}.png",
                        output_image=f"train2017/{domain}__{item_index:03d}.jpg",
                        domain=domain,
                        source_image_id=domain_index * 1000 + item_index,
                        crop_x=0,
                        crop_y=128,
                        width=1024,
                        height=768,
                        source_boxes=item_index,
                        output_sha256="a" * 64,
                    )
                )
            rows.append(
                CalibrationRow(
                    split="val",
                    source_member=f"{domain}/validation.png",
                    output_image=f"val2017/{domain}__validation.jpg",
                    domain=domain,
                    source_image_id=domain_index * 1000 + 999,
                    crop_x=0,
                    crop_y=0,
                    width=1024,
                    height=768,
                    source_boxes=999,
                    output_sha256="b" * 64,
                )
            )
        return rows

    def test_selection_is_train_only_deterministic_and_stratified(self) -> None:
        rows = self._rows()
        expected = select_calibration_rows(rows, count=200)
        shuffled = list(rows)
        random.Random(20260812).shuffle(shuffled)
        observed = select_calibration_rows(shuffled, count=200)

        self.assertEqual(expected, observed)
        self.assertEqual(len(observed), 200)
        self.assertEqual({row.split for row in observed}, {"train"})
        self.assertEqual(len({row.source_member for row in observed}), 200)
        self.assertEqual(
            {(row.domain, row.density_level) for row in observed},
            {
                (f"domain_{domain}", density)
                for domain in range(7)
                for density in range(5)
            },
        )

    def test_selection_rejects_count_that_cannot_cover_all_strata(self) -> None:
        with self.assertRaisesRegex(ValueError, "cannot cover all 35"):
            select_calibration_rows(self._rows(), count=34)

    def test_manifest_loader_rejects_parent_traversal(self) -> None:
        columns = [
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
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "split.csv"
            with path.open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=columns)
                writer.writeheader()
                writer.writerow(
                    {
                        "split": "train",
                        "source_member": "../outside.png",
                        "output_image": "train2017/outside.jpg",
                        "domain": "domain_0",
                        "source_image_id": 1,
                        "crop_x": 0,
                        "crop_y": 0,
                        "width": 1024,
                        "height": 768,
                        "source_boxes": 1,
                        "output_sha256": "c" * 64,
                    }
                )
            with self.assertRaisesRegex(ValueError, "unsafe source member"):
                load_split_manifest(path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
