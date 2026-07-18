#!/usr/bin/env python3
"""GPU-free WP175 boundary contract gates."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


OPENPBR_MANIFEST = Path("src/core/resources/surfaces/openpbr/manifest.json")
OPENPBR_IMPORTER_FIXTURE = Path("test/fixtures/contract0/openpbr_importer_variants.json")
PROJECTION_INVENTORY = Path("test/fixtures/projection_jitter_consumers.json")
ENGINE_MVP_CONTRACT = Path("test/fixtures/contract0/engine_mvp_v1.json")

OPENPBR_PIN = {
    "version": "1.1.1",
    "tag": "v1.1.1",
    "commit": "f8d6d947dfae4c9b599965a86c22826ea7a8dbfb",
}
OPENPBR_DEFINES = {
    "OPENPBR_PIN_V1_1_1",
    "OPENPBR_PIN_F8D6D947DFAE4C9B599965A86C22826EA7A8DBFB",
}
ENGINE_MVP_V1 = {
    "abi": "pelican_surface_v1",
    "symbol": "engineMvp",
    "offset_bytes": 0,
    "size_bytes": 64,
    "value": "RenderFrameSnapshot.view_projection_jittered",
    "input_space": "world",
    "output_space": "jittered_clip",
    "model_application": "before_engineMvp_in_vertex_shader",
    "jitter_application": "projection_before_view",
    "rename_policy": "new_symbol_in_next_abi_only",
}


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def _relative_files(root: Path, extensions: set[str]) -> Iterable[Path]:
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in extensions:
            yield path


def _check_schema(
    value: Any, *, schema: str, version: int, label: str, issues: list[str]
) -> bool:
    if not isinstance(value, dict):
        issues.append(f"{label}: root must be an object")
        return False
    if value.get("schema") != schema:
        issues.append(f"{label}: schema must be {schema!r}")
    if value.get("version") != version:
        issues.append(f"{label}: version must be {version}")
    return True


def exact_set_issues(
    label: str,
    left_name: str,
    left: Iterable[str],
    right_name: str,
    right: Iterable[str],
) -> list[str]:
    """Return stable, name-bearing diagnostics for a two-sided exact-set check."""
    left_set = set(left)
    right_set = set(right)
    issues = [
        f"{label}: {left_name}-only: {name}"
        for name in sorted(left_set - right_set)
    ]
    issues.extend(
        f"{label}: {right_name}-only: {name}"
        for name in sorted(right_set - left_set)
    )
    return issues


def _variant_map(
    variants: Any, *, owner: str, issues: list[str]
) -> dict[str, tuple[str, str, bool]]:
    result: dict[str, tuple[str, str, bool]] = {}
    if not isinstance(variants, list):
        issues.append(f"OpenPBR {owner}: variants must be an array")
        return result
    for index, variant in enumerate(variants):
        if not isinstance(variant, dict):
            issues.append(f"OpenPBR {owner}: variant[{index}] must be an object")
            continue
        try:
            name = variant["name"]
            surface = variant["surface"]
            alpha_mode = variant["alpha_mode"]
            double_sided = variant["double_sided"]
        except KeyError as error:
            issues.append(
                f"OpenPBR {owner}: variant[{index}] missing field {error.args[0]}"
            )
            continue
        if not isinstance(name, str) or not name:
            issues.append(f"OpenPBR {owner}: variant[{index}] has an invalid name")
            continue
        if name in result:
            issues.append(f"OpenPBR {owner}: duplicate variant name: {name}")
            continue
        if not isinstance(surface, str) or not isinstance(alpha_mode, str):
            issues.append(f"OpenPBR {owner}: variant {name} has non-string routing")
            continue
        if not isinstance(double_sided, bool):
            issues.append(f"OpenPBR {owner}: variant {name} double_sided must be boolean")
            continue
        result[name] = (surface, alpha_mode, double_sided)
    return result


def validate_openpbr(repo_root: Path) -> list[str]:
    issues: list[str] = []
    try:
        engine = _load_json(repo_root / OPENPBR_MANIFEST)
        importer = _load_json(repo_root / OPENPBR_IMPORTER_FIXTURE)
    except (OSError, json.JSONDecodeError) as error:
        return [f"OpenPBR contract: cannot load fixture: {error}"]

    _check_schema(
        engine,
        schema="pelican.openpbr_surface",
        version=1,
        label="OpenPBR engine manifest",
        issues=issues,
    )
    if not _check_schema(
        importer,
        schema="pelican.openpbr_importer_variants",
        version=1,
        label="OpenPBR importer fixture",
        issues=issues,
    ):
        return issues

    for owner, value in (("engine", engine), ("importer", importer)):
        actual_pin = value.get("openpbr") if isinstance(value, dict) else None
        if actual_pin != OPENPBR_PIN:
            issues.append(
                f"OpenPBR {owner}: exact pin mismatch: expected {OPENPBR_PIN}, "
                f"got {actual_pin}"
            )

    defines = importer.get("defines", [])
    if not isinstance(defines, list) or set(defines) != OPENPBR_DEFINES or len(defines) != 2:
        issues.append(
            "OpenPBR importer: exact pin defines mismatch: expected "
            + ", ".join(sorted(OPENPBR_DEFINES))
        )

    engine_variants = _variant_map(engine.get("variants"), owner="engine", issues=issues)
    importer_variants = _variant_map(
        importer.get("variants"), owner="importer", issues=issues
    )
    issues.extend(
        exact_set_issues(
            "OpenPBR variants",
            "engine",
            engine_variants,
            "importer",
            importer_variants,
        )
    )
    for name in sorted(set(engine_variants) & set(importer_variants)):
        if engine_variants[name] != importer_variants[name]:
            issues.append(
                f"OpenPBR variant {name}: engine routing {engine_variants[name]} "
                f"!= importer routing {importer_variants[name]}"
            )

    expected_names: set[str] = set()
    for alpha_mode in ("opaque", "mask", "blend"):
        for double_sided in (False, True):
            side = "double" if double_sided else "single"
            name = f"{alpha_mode}_{side}_sided"
            expected_names.add(name)
            expected = (
                f"engine://surfaces/openpbr/{alpha_mode}_{side}.surface",
                alpha_mode,
                double_sided,
            )
            for owner, variants in (
                ("engine", engine_variants),
                ("importer", importer_variants),
            ):
                if name in variants and variants[name] != expected:
                    issues.append(
                        f"OpenPBR {owner} variant {name}: non-canonical routing "
                        f"{variants[name]}"
                    )
    issues.extend(
        exact_set_issues(
            "OpenPBR six-variant product",
            "required",
            expected_names,
            "engine",
            engine_variants,
        )
    )
    issues.extend(
        exact_set_issues(
            "OpenPBR six-variant product",
            "required",
            expected_names,
            "importer",
            importer_variants,
        )
    )

    wrapper_root = repo_root / "src/core/resources/surfaces/openpbr"
    wrapper_uris = {
        f"engine://surfaces/openpbr/{path.name}"
        for path in wrapper_root.glob("*.surface")
    }
    manifest_uris = {record[0] for record in engine_variants.values()}
    issues.extend(
        exact_set_issues(
            "OpenPBR wrappers",
            "filesystem",
            wrapper_uris,
            "manifest",
            manifest_uris,
        )
    )
    return issues


def _projection_discovery(
    repo_root: Path, inventory: dict[str, Any], issues: list[str]
) -> set[str]:
    scan = inventory.get("scan")
    if not isinstance(scan, dict):
        issues.append("projection inventory: scan must be an object")
        return set()
    try:
        patterns = [re.compile(value) for value in scan["patterns"]]
        extensions = set(scan["extensions"])
        roots = scan["roots"]
    except (KeyError, TypeError, re.error) as error:
        issues.append(f"projection inventory: invalid scan configuration: {error}")
        return set()

    discovered: set[str] = set()
    for relative_root in roots:
        source_root = repo_root / relative_root
        if not source_root.is_dir():
            issues.append(f"projection inventory: scan root missing: {relative_root}")
            continue
        for path in _relative_files(source_root, extensions):
            try:
                source = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                issues.append(f"projection inventory: cannot read {path}: {error}")
                continue
            if any(pattern.search(source) for pattern in patterns):
                discovered.add(path.relative_to(repo_root).as_posix())
    return discovered


def validate_projection(repo_root: Path) -> list[str]:
    issues: list[str] = []
    try:
        inventory = _load_json(repo_root / PROJECTION_INVENTORY)
    except (OSError, json.JSONDecodeError) as error:
        return [f"projection inventory: cannot load fixture: {error}"]
    if not _check_schema(
        inventory,
        schema="pelican.projection_consumer_inventory",
        version=1,
        label="projection inventory",
        issues=issues,
    ):
        return issues

    consumers = inventory.get("consumers")
    consumer_names: set[str] = set()
    if not isinstance(consumers, list):
        issues.append("projection inventory: consumers must be an array")
        consumers = []
    for index, consumer in enumerate(consumers):
        if not isinstance(consumer, dict):
            issues.append(f"projection inventory: consumer[{index}] must be an object")
            continue
        name = consumer.get("name")
        if not isinstance(name, str) or not name:
            issues.append(f"projection inventory: consumer[{index}] has invalid name")
            continue
        if name in consumer_names:
            issues.append(f"projection inventory: duplicate consumer: {name}")
        consumer_names.add(name)
        for field in ("matrix", "route", "taa_policy"):
            if not isinstance(consumer.get(field), str) or not consumer[field]:
                issues.append(f"projection consumer {name}: missing {field}")

    sources = inventory.get("sources")
    registered: dict[str, bool] = {}
    covered_consumers: set[str] = set()
    if not isinstance(sources, list):
        issues.append("projection inventory: sources must be an array")
        sources = []
    for index, entry in enumerate(sources):
        if not isinstance(entry, dict):
            issues.append(f"projection inventory: source[{index}] must be an object")
            continue
        source = entry.get("source")
        consumer = entry.get("consumer")
        role = entry.get("role")
        needles = entry.get("needles")
        scanned = entry.get("scan", True)
        if not isinstance(source, str) or not source:
            issues.append(f"projection inventory: source[{index}] has invalid path")
            continue
        if source in registered:
            issues.append(f"projection inventory: duplicate source: {source}")
            continue
        if not isinstance(scanned, bool):
            issues.append(f"projection source {source}: scan must be boolean")
            scanned = True
        registered[source] = scanned
        if consumer not in consumer_names:
            issues.append(
                f"projection source {source}: unknown consumer {consumer!r}"
            )
        else:
            covered_consumers.add(consumer)
        if not isinstance(role, str) or not role:
            issues.append(f"projection source {source}: missing role")
        if not isinstance(needles, list) or not needles or not all(
            isinstance(needle, str) and needle for needle in needles
        ):
            issues.append(f"projection source {source}: needles must be non-empty strings")
            continue
        path = repo_root / source
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            issues.append(f"projection source {source}: cannot read: {error}")
            continue
        for needle in needles:
            if needle not in text:
                issues.append(
                    f"projection source {source}: missing anchor: {needle}"
                )

    for name in sorted(consumer_names - covered_consumers):
        issues.append(f"projection consumer has no source: {name}")

    discovered = _projection_discovery(repo_root, inventory, issues)
    scanned_sources = {source for source, scanned in registered.items() if scanned}
    issues.extend(
        exact_set_issues(
            "projection sources",
            "discovered-unregistered",
            discovered,
            "inventory-only",
            scanned_sources,
        )
    )
    return issues


def validate_engine_mvp(repo_root: Path) -> list[str]:
    issues: list[str] = []
    try:
        fixture = _load_json(repo_root / ENGINE_MVP_CONTRACT)
    except (OSError, json.JSONDecodeError) as error:
        return [f"engineMvp contract: cannot load fixture: {error}"]
    if not _check_schema(
        fixture,
        schema="pelican.engine_mvp_contract",
        version=1,
        label="engineMvp contract",
        issues=issues,
    ):
        return issues
    if fixture.get("contract") != ENGINE_MVP_V1:
        issues.append(
            f"engineMvp contract: v1 semantics changed: expected {ENGINE_MVP_V1}, "
            f"got {fixture.get('contract')}"
        )

    anchors = fixture.get("anchors")
    anchor_paths: set[str] = set()
    if not isinstance(anchors, list):
        issues.append("engineMvp contract: anchors must be an array")
        anchors = []
    for index, anchor in enumerate(anchors):
        if not isinstance(anchor, dict):
            issues.append(f"engineMvp contract: anchor[{index}] must be an object")
            continue
        source = anchor.get("source")
        needles = anchor.get("needles")
        if not isinstance(source, str) or not source:
            issues.append(f"engineMvp contract: anchor[{index}] has invalid source")
            continue
        if source in anchor_paths:
            issues.append(f"engineMvp contract: duplicate anchor source: {source}")
        anchor_paths.add(source)
        if not isinstance(needles, list) or not needles or not all(
            isinstance(needle, str) and needle for needle in needles
        ):
            issues.append(f"engineMvp contract: invalid needles for {source}")
            continue
        try:
            text = (repo_root / source).read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            issues.append(f"engineMvp contract: cannot read {source}: {error}")
            continue
        for needle in needles:
            if needle not in text:
                issues.append(f"engineMvp contract: {source} missing anchor: {needle}")

    shader_root = repo_root / "src/core/resources"
    discovered_shaders = {
        path.relative_to(repo_root).as_posix()
        for path in _relative_files(shader_root, {".vert", ".frag", ".glsl"})
        if "engineMvp" in path.read_text(encoding="utf-8")
    }
    declared_shaders = set(fixture.get("engine_mvp_shader_sources", []))
    issues.extend(
        exact_set_issues(
            "engineMvp shader sources",
            "discovered-unregistered",
            discovered_shaders,
            "fixture-only",
            declared_shaders,
        )
    )
    for source in sorted(declared_shaders - anchor_paths):
        issues.append(f"engineMvp shader source lacks semantic anchor: {source}")
    return issues


VALIDATORS = {
    "openpbr": validate_openpbr,
    "projection": validate_projection,
    "engine-mvp": validate_engine_mvp,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--gate", choices=["all", *VALIDATORS], default="all")
    arguments = parser.parse_args()
    gates = VALIDATORS if arguments.gate == "all" else {arguments.gate: VALIDATORS[arguments.gate]}
    issues: list[str] = []
    for name, validator in gates.items():
        gate_issues = validator(arguments.repo_root.resolve())
        issues.extend(f"{name}: {issue}" for issue in gate_issues)
    if issues:
        for issue in issues:
            print(issue, file=sys.stderr)
        return 1
    print("WP175 contract gate passed: " + ", ".join(gates))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
