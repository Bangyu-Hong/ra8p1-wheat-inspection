#!/usr/bin/env python3
"""Run the frozen YOLOX tile224 package/toolchain/staging preflight."""

from __future__ import annotations

import argparse
from pathlib import Path

from wheat_deployment.yolox_tile224_gate import run_preflight, write_report


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_PACKAGE = (
    REPO_ROOT / "models/8.14/YOLOX_Tiny_GWHD_224_handoff_20260813"
)
DEFAULT_RUHMI_ROOT = Path("F:/Renesas/RUHMI")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, default=DEFAULT_PACKAGE)
    parser.add_argument("--ruhmi-root", type=Path, default=DEFAULT_RUHMI_ROOT)
    parser.add_argument("--staging-root", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    if args.output_dir.exists():
        parser.error(f"output directory already exists: {args.output_dir}")
    staging_root = args.staging_root or args.ruhmi_root / "model_staging"
    args.output_dir.mkdir(parents=True)
    report = run_preflight(args.package_root, args.ruhmi_root, staging_root)
    report_path = args.output_dir / "tile224_preflight.json"
    write_report(report, report_path)
    print(
        f"tile224-preflight status={report['status']} "
        f"package={report['package'].get('verified_entries', 0)}/"
        f"{report['package'].get('expected_entries', 0)} "
        f"staging={report['staging'].get('action')} "
        f"report={report_path}"
    )
    return 0 if report["status"] == "preflight_passed" else 2


if __name__ == "__main__":
    raise SystemExit(main())

