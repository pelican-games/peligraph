"""Thin stdio JSON-RPC client for ``pelican_player``."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import threading
from types import TracebackType
from typing import Any, Self


class PelicanRpcError(RuntimeError):
    """An error response returned by ``pelican_player``."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"Pelican RPC error {code}: {message}")


class PelicanRpc:
    """Launch and control a headless ``pelican_player`` process over stdio."""

    _DEFAULT_EXE = Path("build/src/player/Debug/pelican_player.exe")

    def __init__(
        self,
        project_dir: str | Path,
        exe_path: str | Path | None = None,
    ) -> None:
        self.project_dir = Path(project_dir).expanduser().resolve()
        self.exe_path = self._resolve_executable(exe_path)
        self._next_id = 1
        self._call_lock = threading.Lock()
        self._closed = False
        self._process = subprocess.Popen(
            [
                str(self.exe_path),
                "--rpc",
                "--headless",
                "--project",
                str(self.project_dir),
            ],
            cwd=self.exe_path.parent,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )

    @classmethod
    def _resolve_executable(cls, exe_path: str | Path | None) -> Path:
        if exe_path is not None:
            explicit = Path(exe_path).expanduser().resolve()
            if not explicit.is_file():
                raise FileNotFoundError(f"pelican_player executable not found: {explicit}")
            return explicit

        repository_root = Path(__file__).resolve().parent.parent
        search_roots = [repository_root, Path.cwd().resolve(), *Path.cwd().resolve().parents]
        searched: list[Path] = []
        for root in search_roots:
            candidate = root / cls._DEFAULT_EXE
            if candidate in searched:
                continue
            searched.append(candidate)
            if candidate.is_file():
                return candidate

        locations = "\n".join(f"  {path}" for path in searched)
        raise FileNotFoundError(
            "pelican_player executable was not found. Searched:\n" + locations
        )

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        """Call one engine method and return its JSON ``result`` unchanged."""

        with self._call_lock:
            if self._closed:
                raise RuntimeError("Pelican RPC client is closed")
            if self._process.poll() is not None:
                raise RuntimeError(
                    f"pelican_player exited with code {self._process.returncode}"
                )

            request_id = self._next_id
            self._next_id += 1
            request = {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": method,
                "params": {} if params is None else params,
            }
            request_line = json.dumps(request, ensure_ascii=False, separators=(",", ":"))

            stdin = self._process.stdin
            stdout = self._process.stdout
            if stdin is None or stdout is None:
                raise RuntimeError("pelican_player stdio is unavailable")

            try:
                stdin.write(request_line + "\n")
                stdin.flush()
            except (BrokenPipeError, OSError) as error:
                raise RuntimeError("failed to write to pelican_player") from error

            response_line = stdout.readline()
            if response_line == "":
                return_code = self._process.poll()
                detail = (
                    f" with code {return_code}" if return_code is not None else " unexpectedly"
                )
                raise RuntimeError(f"pelican_player closed RPC stdout{detail}")

            try:
                response = json.loads(response_line)
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    f"pelican_player returned invalid JSON: {response_line.rstrip()}"
                ) from error

            if not isinstance(response, dict) or response.get("jsonrpc") != "2.0":
                raise RuntimeError(f"invalid JSON-RPC response: {response!r}")
            if response.get("id") != request_id:
                raise RuntimeError(
                    f"JSON-RPC response id mismatch: expected {request_id}, "
                    f"got {response.get('id')!r}"
                )
            if "error" in response:
                error = response["error"]
                if not isinstance(error, dict):
                    raise RuntimeError(f"invalid JSON-RPC error response: {response!r}")
                raise PelicanRpcError(
                    error.get("code"),
                    error.get("message", ""),
                    error.get("data"),
                )
            if "result" not in response:
                raise RuntimeError(f"JSON-RPC response has no result: {response!r}")
            return response["result"]

    def get_status(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("get_status", params)

    def step_frame(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("step_frame", params)

    def set_time(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("set_time", params)

    def render_frame(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("render_frame", params)

    def capture(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("capture", params)

    def load_scene(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("load_scene", params)

    def scene_tree(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("scene_tree", params)

    def get_components(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("get_components", params)

    def list_assets(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("list_assets", params)

    def export_scene_snapshot(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("export_scene_snapshot", params)

    def import_scene_snapshot(self, params: dict[str, Any] | None = None) -> Any:
        return self.call("import_scene_snapshot", params)

    def eval_preview(self, params: dict[str, Any] | None = None) -> Any:
        """Evaluate request-local scene overrides without publishing them."""
        return self.call("eval_preview", params)

    def render_preview(self, params: dict[str, Any] | None = None) -> Any:
        """Capture the isolated preview graph and return its response unchanged."""
        return self.call("render_preview", params)

    def terminate(self) -> None:
        """Terminate the player and wait until no child process remains."""

        with self._call_lock:
            if self._closed:
                return
            self._closed = True
            if self._process.poll() is None:
                self._process.terminate()
                try:
                    self._process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait()
            else:
                self._process.wait()

            for stream in (self._process.stdin, self._process.stdout):
                if stream is not None:
                    stream.close()

    close = terminate

    def __enter__(self) -> Self:
        if self._closed:
            raise RuntimeError("Pelican RPC client is closed")
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.terminate()
