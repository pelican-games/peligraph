#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mkdocs==1.6.1", "mkdocs-material==9.6.22"]
# ///
"""Build or preview the documentation: uv run tools/docs_site.py [build|serve]."""
import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "serve"), nargs="?", default="build")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    command = [sys.executable, "-m", "mkdocs", args.command, "--config-file", str(root / "mkdocs.yml")]
    if args.command == "build":
        command.append("--strict")
    return subprocess.call(command, cwd=root)


if __name__ == "__main__":
    sys.exit(main())
