#!/usr/bin/env python3
"""Audit generated YOLOX tile224 MERA/Vela allocation and memory evidence."""

from __future__ import annotations

import argparse
from pathlib import Path

from wheat_deployment.yolox_tile224_gate import (
    inspect_compiler_output,
    write_report,
)


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MEMORY_REGIONS = REPO_ROOT / "workspace/camera/Release/memory_regions.ld"
DEFAULT_BSP_CONFIG = (
    REPO_ROOT / "workspace/camera/ra_cfg/fsp_cfg/bsp/bsp_mcu_family_cfg.h"
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler-output-dir", required=True, type=Path)
    parser.add_argument(
        "--firmware-memory-regions", type=Path, default=DEFAULT_MEMORY_REGIONS
    )
    parser.add_argument("--compile-log", type=Path)
    parser.add_argument("--firmware-bsp-config", type=Path, default=DEFAULT_BSP_CONFIG)
    parser.add_argument("--output-report", required=True, type=Path)
    args = parser.parse_args(argv)

    if args.output_report.exists():
        parser.error(f"output report already exists: {args.output_report}")
    report = inspect_compiler_output(
        args.compiler_output_dir,
        args.firmware_memory_regions,
        compile_log=args.compile_log,
        firmware_bsp_config=args.firmware_bsp_config,
    )
    write_report(report, args.output_report)
    print(
        f"tile224-allocation status={report['status']} "
        f"cpu={report.get('allocation', {}).get('cpu_region_count', 0)} "
        f"npu={report.get('allocation', {}).get('npu_region_count', 0)} "
        f"firmware_allowed={report['firmware_integration_allowed']} "
        f"report={args.output_report}"
    )
    return 0 if report["status"] == "host_gate_qualified" else 3


if __name__ == "__main__":
    raise SystemExit(main())
