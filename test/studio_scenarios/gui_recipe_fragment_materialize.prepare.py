"""Prepare a private experimental recipe project for the GUI scenario."""
import argparse
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--cli", required=True, type=Path)
args = parser.parse_args()
project = args.out / "recipe"
subprocess.run([str(args.cli), "project", "init", str(project), "--template", "blank"], check=True)
path = project / "passes/main_rendering_config.json"
root = json.loads(path.read_text(encoding="utf-8"))
root["nodes"] = []
root["rendering"] = {"pipeline": {"preset": "engine://render_pipelines/experimental/hybrid_recipe.json"}}
path.write_text(json.dumps(root, indent=2) + "\n", encoding="utf-8")
