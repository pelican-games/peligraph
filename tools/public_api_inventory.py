#!/usr/bin/env python3
"""Generate/check the exact Pelican public context API inventory."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HEADERS = (
    ("src/core/userpublic/gamecontext.hpp", "PhysicsContext"),
    ("src/core/userpublic/gamecontext.hpp", "AudioContext"),
    ("src/core/userpublic/gamecontext.hpp", "AnimationContext"),
    ("src/core/userpublic/gamecontext.hpp", "SaveContext"),
    ("src/core/userpublic/gamecontext.hpp", "RenderContext"),
    ("src/core/userpublic/gamecontext.hpp", "ObjectsContext"),
    ("src/core/userpublic/gamecontext.hpp", "GameContext"),
    ("src/core/userpublic/gameobjects.hpp", "GameObjects"),
    ("src/core/userpublic/behavior.hpp", "BehaviorContext"),
)
OUTPUT = Path("test/fixtures/public_api_inventory.json")
RUNTIME_MANIFEST = Path("src/core/loader/sceneparticipantmanifest.cpp")

MUTATORS = {
    "createObject", "createSpriteObject", "removeObject", "setLocalTransform",
    "setSpriteView", "setSpriteTexture", "setDirectionalLightDirection",
    "setDirectionalLightIntensity", "setPointLightPosition",
    "setSpotLightDirection", "setCamera", "random", "randomInt",
    "randomFloat", "setSeed", "playSound", "stopSound", "setBusVolume",
    "loadScene", "debugText", "setGameSettings", "saveSettings", "saveData",
    "add", "createWithComponents", "remove", "removeAll",
}
HOST_ENTRANCES = {"emit"}
NON_MUTATING_VOID = {"logInfo", "logWarning", "logError"}

def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def class_body(text: str, name: str) -> str:
    match = re.search(rf"class(?:\s+PELICAN_API)?\s+{name}\b[^{{]*{{", text)
    if not match:
        raise ValueError(f"class not found: {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
    raise ValueError(f"unterminated class: {name}")


def normalize(statement: str) -> str:
    statement = re.sub(r"\s+", " ", statement).strip()
    statement = re.sub(r"\s*([(),&*<>])\s*", r"\1", statement)
    statement = re.sub(r"\s*=\s*", " = ", statement)
    return statement.rstrip(";").strip()


def public_functions(body: str) -> list[str]:
    functions: list[str] = []
    access = "private"
    cursor = 0
    statement_start = 0
    paren_depth = 0
    while cursor < len(body):
        label = None if paren_depth else re.match(
            r"\s*(public|private|protected)\s*:", body[cursor:])
        if label:
            access = label.group(1)
            cursor += label.end()
            statement_start = cursor
            continue
        ch = body[cursor]
        if ch == "(":
            paren_depth += 1
        elif ch == ")":
            paren_depth -= 1
            if paren_depth < 0:
                raise ValueError("unbalanced ')' in public API declaration")
        elif ch == "{" and paren_depth == 0:
            prefix = body[statement_start:cursor].strip()
            depth = 1
            cursor += 1
            while cursor < len(body) and depth:
                depth += body[cursor] == "{"
                depth -= body[cursor] == "}"
                cursor += 1
            if access == "public" and "(" in prefix and not prefix.startswith("struct "):
                functions.append(normalize(prefix))
            statement_start = cursor
            continue
        if ch == ";" and paren_depth == 0:
            statement = body[statement_start:cursor].strip()
            if access == "public" and "(" in statement:
                functions.append(normalize(statement))
            cursor += 1
            statement_start = cursor
            continue
        cursor += 1
    return functions


def method_name(signature: str) -> str:
    matches = list(re.finditer(r"(?:~?[A-Za-z_]\w*)\(", signature))
    if not matches:
        raise ValueError(f"function name not found: {signature}")
    return matches[0].group(0)[:-1]


def runtime_manifest(root: Path) -> dict[tuple[str, str], str]:
    text = (root / RUNTIME_MANIFEST).read_text(encoding="utf-8")
    start = text.index("constexpr std::array public_api_manifest{")
    end = text.index("\n};", start)
    rows = re.findall(
        r'ScenePublicApiManifestEntry\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}',
        text[start:end],
    )
    result: dict[tuple[str, str], str] = {}
    for class_name, signature, classification in rows:
        key = (class_name, signature)
        if key in result:
            raise ValueError(f"duplicate runtime manifest API: {class_name}::{signature}")
        result[key] = classification
    if not result:
        raise ValueError("runtime public API manifest is empty")
    return result


def generate(root: Path) -> dict:
    manifest = runtime_manifest(root)
    consumed: set[tuple[str, str]] = set()
    rows: list[dict[str, str]] = []
    for relative, class_name in HEADERS:
        text = strip_comments((root / relative).read_text(encoding="utf-8"))
        for signature in public_functions(class_body(text, class_name)):
            name = method_name(signature)
            looks_mutating = (
                name not in NON_MUTATING_VOID and (
                    signature.startswith("void ") or
                    re.match(r"(?:template<[^>]+>)?(?:static )?(?:auto|bool|GameObjectId|SoundHandle) "
                             r"(?:set|save|create|remove|play|stop|load|random|debug|add)",
                             signature) is not None
                )
            )
            classification = (
                "host_entrance" if name in HOST_ENTRANCES
                else "mutator" if name in MUTATORS or looks_mutating
                else "getter"
            )
            row = {
                "header": relative,
                "class": class_name,
                "signature": signature,
                "classification": classification,
            }
            if classification in {"mutator", "host_entrance"}:
                key = (class_name, signature)
                manifest_classification = manifest.get(key)
                if not manifest_classification:
                    raise ValueError(f"mutator lacks manifest classification: {class_name}::{signature}")
                consumed.add(key)
                row["manifest"] = manifest_classification
            rows.append(row)
    extras = sorted(set(manifest) - consumed)
    if extras:
        rendered = ", ".join(f"{owner}::{signature}" for owner, signature in extras)
        raise ValueError(f"runtime manifest API absent from inventory: {rendered}")
    return {"schema": "pelican.public_api_inventory", "version": 1,
            "entries": rows}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    actual = generate(args.repo_root)
    output = args.repo_root / OUTPUT
    encoded = json.dumps(actual, indent=2, ensure_ascii=False) + "\n"
    if args.write:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
        print(output)
        return 0
    if not output.exists() or output.read_text(encoding="utf-8") != encoded:
        print("public API inventory is stale; run tools/public_api_inventory.py --write")
        return 1
    print("public API inventory: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
