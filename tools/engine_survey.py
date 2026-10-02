"""Reproducible Release engine survey. Run the whole batch under the GPU lease.

Only private fixture copies are edited. RPC wall times include pipe transport;
client JSON decoding is measured separately. No claimed engine-only subtraction.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import statistics
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def stats(values):
    return dict(samples=len(values), median_ms=statistics.median(values),
                p95_ms=sorted(values)[math.ceil(.95 * len(values)) - 1],
                min_ms=min(values), max_ms=max(values), values_ms=values)


class Rpc:
    def __init__(self, player, project, directory, size):
        self.directory = directory
        self.stderr = (directory / "rpc-stderr.log").open("ab")
        self.started = time.perf_counter()
        self.process = subprocess.Popen(
            [str(player), "--headless", "--rpc", "--project", str(project),
             "--size", size, "--start-state", "edit", "--audio-backend", "offline",
             "--vulkan-validation", "off"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr)
        self.responses = queue.Queue()
        self.next_id = 0
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.responses.put((time.perf_counter(), line))
        self.responses.put((time.perf_counter(), b""))

    def call(self, method, params=None):
        self.next_id += 1
        payload = json.dumps(dict(jsonrpc="2.0", id=self.next_id, method=method,
                                  params=params or {}), separators=(",", ":")).encode() + b"\n"
        start = time.perf_counter()
        self.process.stdin.write(payload)
        self.process.stdin.flush()
        received, line = self.responses.get(timeout=120)
        if not line:
            raise RuntimeError(f"engine exited ({self.process.poll()}); see rpc-stderr.log")
        parse_start = time.perf_counter()
        response = json.loads(line)
        parse_ms = (time.perf_counter() - parse_start) * 1000
        if response.get("id") != self.next_id:
            raise RuntimeError(f"RPC id mismatch: {response}")
        if "error" in response:
            raise RuntimeError(f"{method}: {response['error']}")
        return response["result"], (received-start)*1000, parse_ms, len(line)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=2)
        self.stderr.close()
        return self.process.returncode


def measure(client, method, params=None, samples=15, warmup=3):
    wire, parse, sizes = [], [], []
    for i in range(warmup+samples):
        value, ms, parse_ms, size = client.call(method, params)
        if i >= warmup:
            wire.append(ms)
            parse.append(parse_ms)
            sizes.append(size)
    return dict(**stats(wire), client_parse=stats(parse), response_bytes_median=statistics.median(sizes)), value


def prepare(source, destination, count=None, cubes=False, timing=True):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(".pelican", "build"))
    config = json.loads((destination/"project.json").read_text(encoding="utf-8"))
    graph_path = destination/config["basic_config"]["rendering_config_json"]
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    features = graph["rendering"].setdefault("features", [])
    if timing and "engine://features/gpu_timing.json" not in features:
        features.append("engine://features/gpu_timing.json")
    save(graph_path, graph)
    if count is not None:
        scene_path = destination/config["basic_config"]["scene_data_json"]
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        objects = scene["scenes"]["default_scene"]["objects"]
        template = copy.deepcopy(objects[0])
        for i in range(count-len(objects)):
            obj = copy.deepcopy(template)
            obj["name"] = f"Object_{i:05}"
            obj["components"][0]["pos"] = [(i % 32)*.2, (i//32 % 32)*.2, -(i//1024)*.2]
            if cubes:
                obj["components"][0]["scale"] = [.1, .1, .1]
                obj["components"].append(dict(name="simplemodelview", model="engine://primitives/cube"))
            objects.append(obj)
        save(scene_path, scene)
    return config


def run_case(player, directory, source, count, cubes, size, timing=True):
    directory.mkdir()
    project = directory/"project"
    config = prepare(source, project, count, cubes, timing)
    result = dict(name=directory.name, source=str(source.relative_to(ROOT)), objects=count,
                  cubes=cubes, resolution=size, timing_enabled=timing, operations={}, failures=[])
    client = None
    try:
        client = Rpc(player, project, directory, size)
        status = client.call("get_status")[0]
        result["launch_to_first_status_ms"] = (time.perf_counter()-client.started)*1000
        result["startup_first"] = status["startup"]
        result["device"] = status.get("device", status.get("vulkan", {}))
        save(directory/"status-start.json", status)
        for _ in range(8):
            client.call("step_frame")
        methods = ["get_scene_revision", "scene_tree", "get_status", "get_frame_plan",
                   "get_gpu_timing", "list_assets", "export_scene_snapshot",
                   "get_graph_instances", "get_graph_values", "get_graph_catalog",
                   "get_render_authoring_context", "get_render_documents",
                   "get_engine_shaders", "get_registered_providers", "get_runtime_objects",
                   "get_render_features", "get_frame_node_order"]
        for method in methods:
            try:
                params = {"schema_version": 2} if method == "export_scene_snapshot" else ({"instance": "left"} if method == "get_graph_values" else None)
                result["operations"][method], value = measure(client, method, params)
                if params:
                    result["operations"][method]["query_params"] = params
                if method == "scene_tree":
                    tree = value
                    result["actual_objects"] = len(tree["objects"])
                if method == "get_frame_plan":
                    save(directory/"frame-plan.json", value)
            except Exception as e:
                result["failures"].append(str(e)[:1500])
        if "tree" in locals() and tree["objects"]:
            for position in [0, len(tree["objects"])-1]:
                params = dict(scene_id=tree["scene_id"], authoring_object_id=tree["objects"][position]["authoring_object_id"])
                result["operations"][f"get_components_{position}"], _ = measure(client, "get_components", params)
            actor = client.call("open_editor_session", dict(display_name="Engine performance survey"))[0]["actor_id"]
            stages = {"edit_enqueue":[], "edit_apply_and_frame":[], "edit_result":[]}
            for i in range(8):
                revision = client.call("get_scene_revision")[0]["scene_revision"]
                request = dict(actor_id=actor, base_revision=revision, operations=[dict(
                    op="set_component_value", object_id=tree["objects"][0]["authoring_object_id"],
                    component_slot="transform", field_path="/pos", value=[float(i % 2),0.,0.])])
                accepted, submit_ms, _, _ = client.call("edit", request)
                _, apply_ms, _, _ = client.call("step_frame")
                committed, result_ms, _, _ = client.call("get_edit_result", dict(ticket=accepted["ticket"]))
                if committed.get("status") != "committed":
                    raise RuntimeError(f"edit did not commit: {committed}")
                if i >= 3:
                    for name, ms in zip(stages, [submit_ms,apply_ms,result_ms]): stages[name].append(ms)
            for name, values in stages.items(): result["operations"][name] = stats(values)
            result["operations"]["save_scene"], _ = measure(client,"save_scene", samples=5, warmup=1)
        result["operations"]["step_frame"], _ = measure(client,"step_frame",samples=40,warmup=8)
        save(directory/"gpu-timing.json", client.call("get_gpu_timing")[0])
        save(directory/"status-end.json", client.call("get_status")[0])
        result["operations"]["render_frame"], _ = measure(client,"render_frame",samples=20)
        try:
            result["operations"]["load_scene"], _ = measure(client,"load_scene",dict(name=config["basic_config"]["default_scene_id"]),samples=5,warmup=1)
            reloaded = client.call("scene_tree")[0]
            result["post_load_object_count"] = len(reloaded["objects"])
            if result["post_load_object_count"] != result["actual_objects"]:
                raise RuntimeError("reload changed the object count")
            client.call("step_frame")
        except Exception as e:
            result["failures"].append(str(e))
        result["ok"] = not result["failures"]
        result["ready_for_offline"] = True
    except Exception as e:
        result["ok"] = False
        result["failures"].append(f"case: {e}")
    finally:
        if client:
            result["rpc_exit_code"] = client.close()
    # Offline frame loop owns CPU timing; RPC step_frame does not record it.
    if result.get("ready_for_offline"):
        runs=[]
        for repeat in range(2):
            log=directory/f"offline-{repeat}.log"
            start=time.perf_counter()
            with log.open("wb") as stream:
                completed=subprocess.run([str(player),"--headless","--project",str(project),
                    "--frames","180","--size",size,"--audio-backend","offline",
                    "--vulkan-validation","off"],stdout=stream,stderr=subprocess.STDOUT,timeout=180)
            text=log.read_text(encoding="utf-8",errors="replace")
            frames=[dict(frames=int(m[0]),update_ms=float(m[1]),render_ms=float(m[2]),pace_wait_ms=float(m[3]))
                    for m in re.findall(r"pelican frame metrics frames=(\d+) cpu_update_ms_avg=([\d.]+) cpu_render_ms_avg=([\d.]+) cpu_present_wait_ms_avg=([\d.]+)",text)]
            runs.append(dict(exit_code=completed.returncode,total_process_ms=(time.perf_counter()-start)*1000,cpu_frame_blocks=frames))
        result["offline_runs"]=runs
        client=Rpc(player,project,directory,size)
        try:
            result["startup_cached"]=client.call("get_status")[0]["startup"]
            result["cached_launch_ms"]=(time.perf_counter()-client.started)*1000
        finally:
            client.close()
    result["ok"] = result["ok"] and not result["failures"] and all(run["exit_code"] == 0 for run in result.get("offline_runs", []))
    save(directory/"result.json",result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player",type=Path,required=True)
    parser.add_argument("--out",type=Path,required=True)
    parser.add_argument("--only",help="Comma-separated case names for a targeted rerun")
    args=parser.parse_args()
    player=args.player.resolve(); out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    metadata=dict(revision=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
        player=str(player),sha256=hashlib.sha256(player.read_bytes()).hexdigest(),
        python=os.sys.version,configuration="Release",gpu_lease="caller must hold the shared GPU lease",
        limits=["Headless RPC has no interactive frame-boundary wait", "GPU timing enabled except explicit control",
                "RPC time includes pipe scheduling and engine serialization; Python decode reported separately",
                "Offline CPU render time includes GPU synchronization; not pure CPU compute",
                "First project shader cache is empty; OS/driver caches are not flushed",
                "Startup and frame-loop repeats are exploratory samples, not confidence intervals"])
    save(out/"environment.json",metadata)
    fixture=ROOT/"test/fixtures/studio_automation_graph"
    cases=[(f"transforms_{n}",fixture,n,False,"640x360",True) for n in [2,100,1000,10000]]
    cases += [(f"cubes_{n}",fixture,n,True,"640x360",True) for n in [100,1000]]
    cases += [(f"resolution_{size}",fixture,2,False,size,True) for size in ["1280x720","1920x1080"]]
    cases += [("timing_off",fixture,2,False,"640x360",False)]
    cases += [(p.name,p,None,False,"960x540",True) for p in sorted((ROOT/"projects").iterdir()) if (p/"project.json").exists()]
    if args.only:
        unknown = set(args.only.split(",")) - {case[0] for case in cases}
        if unknown:
            parser.error(f"Unknown cases: {sorted(unknown)}")
    results=[]
    for name,source,count,cubes,size,timing in cases:
        if args.only and name not in args.only.split(","):continue
        print(f"START {name}",flush=True)
        try:
            result=run_case(player,out/name,source,count,cubes,size,timing)
        except Exception as e:
            result=dict(name=name,ok=False,failures=[str(e)])
            save(out/name/"result.json",result)
        results.append(result); save(out/"results.json",results)
        print(f"DONE {name} ok={result['ok']} failures={len(result.get('failures',[]))}",flush=True)
    with (out/"rpc-summary.csv").open("w",newline="",encoding="utf-8") as stream:
        writer=csv.writer(stream); writer.writerow(["case","operation","samples","median_ms","p95_ms","bytes"])
        for result in results:
            for name,value in result.get("operations",{}).items():
                writer.writerow([result["name"],name,value["samples"],value["median_ms"],value["p95_ms"],value.get("response_bytes_median","")])


if __name__=="__main__":main()
