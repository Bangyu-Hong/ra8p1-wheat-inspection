#!/usr/bin/env python3
"""Thin executable wrapper for wheat_deployment.cli."""

from wheat_deployment.cli import main


if __name__ == "__main__":
    raise SystemExit(main())
