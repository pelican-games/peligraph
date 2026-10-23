"""JP-1: real CLI/player processes, Japanese project and model paths."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--cli-only", action="store_true")
    args = parser.parse_args()

    def run(command, data=None):
        # The surrounding CTest timeout owns the process tree on a hang.
        result = subprocess.run([str(x) for x in command], input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout = result.stdout.decode("utf-8", errors="strict")
        stderr = result.stderr.decode("utf-8", errors="replace")
        assert result.returncode == 0, (command, result.returncode, stdout, stderr)
        return stdout

    with tempfile.TemporaryDirectory(prefix="pelican_jp1_") as temporary:
        root = Path(temporary) / "日本語 プロジェクト"
        run([args.cli, "project", "init", root, "--template", "blank"])
        model = root / "assets/models/木箱.glb"
        shutil.copyfile(args.model, model)
        shutil.copyfile(args.model, root / "assets/models/ソ表.glb")
        assets = root / "assets/asset_data.json"
        document = json.loads(assets.read_text(encoding="utf-8"))
        document["models"] = [{"name": "木箱", "path": "project://assets/models/木箱.glb"}]
        assets.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        project_file = root / "project.json"
        project = json.loads(project_file.read_text(encoding="utf-8"))
        project["asset_stores"] = {"main": {"mount": "assets", "manifest": "assets.manifest.json"}}
        project_file.write_text(json.dumps(project, ensure_ascii=False), encoding="utf-8")
        run([args.cli, "assets", "manifest", "--project", root])
        manifest = json.loads((root / "assets.manifest.json").read_text(encoding="utf-8"))
        for name in ("models/木箱.glb", "models/ソ表.glb"):
            assert name in json.dumps(manifest, ensure_ascii=False), manifest
        run([args.cli, "assets", "verify", "--project", root, "--full"])
        if args.cli_only:
            print("JP-1 CLI: Japanese project, manifest and verification passed")
            return

        requests = [("get_status", {}),
                    ("load_gltf", {"path": "project://assets/models/木箱.glb", "name": "loaded_box"}),
                    ("load_gltf", {"path": "project://assets/models/存在しないソ表.glb"}),
                    ("get_status", {})]
        wire = "\n".join(json.dumps({"jsonrpc": "2.0", "id": index, "method": method, "params": params},
                                  ensure_ascii=False)
                         for index, (method, params) in enumerate(requests, 1)) + "\n"
        stdout = run([args.player, "--rpc", "--headless", "--project", root, "--size", "64x64"],
                     wire.encode("utf-8"))
        replies = [json.loads(line) for line in stdout.splitlines() if line.strip()]
        assert len(replies) == 4, replies
        status = replies[0]["result"]
        assert Path(status["project_root"]) == root.resolve(), status
        assert "日本語 プロジェクト" in status["project_root"], status
        assert replies[1]["result"]["path"].endswith("/木箱.glb"), replies[1]
        assert "存在しないソ表.glb" in replies[2]["error"]["message"], replies[2]
        assert replies[3]["result"]["project_root"] == status["project_root"], replies[3]
        assert json.loads(assets.read_text(encoding="utf-8"))["models"] == document["models"]
        print("JP-1 headless: startup model, get_status, project:// load and error recovery passed")


if __name__ == "__main__":
    main()
