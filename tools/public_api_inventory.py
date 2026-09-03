#!/usr/bin/env python3
"""Generate/check the exact Pelican public context API inventory."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HEADERS = (
    ("src/core/userpublic/gamecontext.hpp", "GameContext"),
    ("src/core/userpublic/gameobjects.hpp", "GameObjects"),
    ("src/core/userpublic/behavior.hpp", "BehaviorContext"),
)
OUTPUT = Path("test/fixtures/public_api_inventory.json")

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

MANIFEST = {
    ("GameContext", "GameObjectId createObject(const LocalTransformComponent&transform)const"): "covered.deferred-object",
    ("GameContext", "GameObjectId createSpriteObject(const LocalTransformComponent&transform,const SpriteViewComponent&sprite)const"): "covered.deferred-object",
    ("GameContext", "[[nodiscard]] bool removeObject(GameObjectId id)const"): "covered.deferred-object",
    ("GameContext", "[[nodiscard]] bool setLocalTransform(GameObjectId id,const LocalTransformComponent&transform)const"): "covered.candidate-or-retired-reject",
    ("GameContext", "[[nodiscard]] bool setSpriteView(GameObjectId id,const SpriteViewComponent&sprite)const"): "covered.candidate-or-retired-reject",
    ("GameContext", "[[nodiscard]] bool setSpriteTexture(GameObjectId id,std::string_view texture)const"): "covered.candidate-or-retired-reject",
    ("GameContext", "[[nodiscard]] bool setDirectionalLightDirection(std::string_view name,vec3 direction)const"): "covered.candidate",
    ("GameContext", "[[nodiscard]] bool setDirectionalLightIntensity(std::string_view name,float intensity)const"): "covered.candidate",
    ("GameContext", "[[nodiscard]] bool setPointLightPosition(std::string_view name,vec3 position)const"): "covered.candidate",
    ("GameContext", "[[nodiscard]] bool setSpotLightDirection(std::string_view name,vec3 direction)const"): "covered.candidate",
    ("GameContext", "void setCamera(std::string_view name)const"): "covered.candidate",
    ("GameContext", "double random()"): "covered.rng-sequence",
    ("GameContext", "int randomInt(int min,int max)"): "covered.rng-sequence",
    ("GameContext", "float randomFloat(float min,float max)"): "covered.rng-sequence",
    ("GameContext", "void setSeed(std::uint64_t seed)"): "covered.rng-sequence",
    ("GameContext", "SoundHandle playSound(std::string_view path)const"): "shadow_outbox.audio-play",
    ("GameContext", "void stopSound(SoundHandle handle)const"): "shadow_outbox.audio-stop",
    ("GameContext", "void setBusVolume(std::string_view bus,float volume)const"): "shadow_outbox.audio-bus-volume",
    ("GameContext", "void loadScene(std::string_view name)const"): "covered.pending-scene",
    ("GameContext", "void debugText(int x,int y,std::string_view text)const"): "nonparticipant.frame-lifetime",
    ("GameContext", "template<class Event>void emit(const Event&event)const"): "covered.event-outbox",
    ("GameContext", "void setGameSettings(Json settings)const"): "shadow_outbox.game-settings",
    ("GameContext", "void saveSettings()const"): "shadow_outbox.save-settings",
    ("GameContext", "void saveData(std::string_view slot,const Json&data)const"): "shadow_outbox.save-data",
    ("GameObjects", "static auto add()"): "reject.direct-gameobjects",
    ("GameObjects", "static GameObjectId createWithComponents(std::span<const ComponentId>ids,const std::function<void(std::span<void*>)>&populate)"): "reject.direct-gameobjects",
    ("GameObjects", "[[nodiscard]] static bool remove(GameObjectId id)"): "reject.direct-gameobjects",
    ("GameObjects", "static void removeAll()"): "reject.direct-gameobjects",
    ("GameObjects", "[[nodiscard]] static bool setLocalTransform(GameObjectId id,const LocalTransformComponent&transform)"): "reject.direct-gameobjects",
    ("BehaviorContext", "GameObjectId createObject(const LocalTransformComponent&transform)const"): "covered.deferred-object",
    ("BehaviorContext", "GameObjectId createSpriteObject(const LocalTransformComponent&transform,const SpriteViewComponent&sprite)const"): "covered.deferred-object",
    ("BehaviorContext", "[[nodiscard]] bool removeObject(GameObjectId id)const"): "covered.deferred-object",
}


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


def generate(root: Path) -> dict:
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
                manifest = MANIFEST.get((class_name, signature))
                if not manifest:
                    raise ValueError(f"mutator lacks manifest classification: {class_name}::{signature}")
                row["manifest"] = manifest
            rows.append(row)
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
