// Coverage gate for pelican.ui_semantic_fixture normative fixtures.
// Dependency-free (node >= 18). Run: node check_coverage.mjs [repo_root]
// Checks (design_ui_2d_foundation.md v8 §7):
//  1. coverage.json conforms to pelican.ui_semantic_coverage.schema.json
//     (structural check implemented natively: closed groups, exact required
//     key match, entry shape).
//  2. Every referenced fixture file exists.
//  3. Machine-extracted enum sets from the fixture schema EXACTLY equal the
//     manifest's enum_coverage keys (set equality, not subset).
//  4. ui_key / ui_pad_control $defs enums are in sync with the
//     consumed_control pattern alternations.
// JSON Schema classification and semantic invariant execution are owned by
// the U0 C++ validator; ajv remains a development cross-check.

import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = process.argv[2] ?? path.resolve(here, "../../../..");
const fixtureSchema = JSON.parse(fs.readFileSync(path.join(repo, "docs/schemas/pelican.ui_semantic_fixture.schema.json"), "utf8"));
const coverageSchema = JSON.parse(fs.readFileSync(path.join(repo, "docs/schemas/pelican.ui_semantic_coverage.schema.json"), "utf8"));
const clauseRegistry = JSON.parse(fs.readFileSync(path.join(repo, "docs/schemas/pelican.ui_semantic_clauses.json"), "utf8"));
const manifest = JSON.parse(fs.readFileSync(path.join(here, "coverage.json"), "utf8"));

let failures = 0;
const fail = (msg) => { failures++; console.error("FAIL:", msg); };
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const setEq = (name, actual, expected) => {
  const a = [...actual].sort(), e = [...expected].sort();
  if (!eq(a, e)) {
    fail(`${name}: set mismatch\n  manifest: ${a.join(", ")}\n  schema:   ${e.join(", ")}`);
  }
};

// --- 1. structural validation of manifest against coverage schema ---
const checkEntry = (where, entry) => {
  if (typeof entry !== "object" || entry === null) return fail(`${where}: entry is not an object`);
  const keys = Object.keys(entry);
  for (const k of keys) if (!["fixture", "gate", "clause"].includes(k)) fail(`${where}: unknown entry key '${k}'`);
  if (!/^(valid|invalid|semantic_invalid)\/[a-z0-9_]+\.json$/.test(entry.fixture ?? "")) fail(`${where}: bad fixture path '${entry.fixture}'`);
  if (!["schema", "semantic"].includes(entry.gate)) fail(`${where}: bad gate '${entry.gate}'`);
  if (entry.clause !== undefined && (typeof entry.clause !== "string" || entry.clause.length > 200)) fail(`${where}: clause must be a string of at most 200 characters`);
  if (entry.gate === "semantic" && entry.fixture?.startsWith("invalid/")) fail(`${where}: semantic gate cannot reference schema-invalid directory`);
  if (entry.gate === "schema" && entry.fixture?.startsWith("semantic_invalid/")) fail(`${where}: schema gate cannot reference semantic-invalid directory`);
};
const checkClosedGroup = (name, group, requiredKeys) => {
  if (typeof group !== "object" || group === null) return fail(`${name}: missing`);
  setEq(`${name} (closed group)`, Object.keys(group), requiredKeys);
  for (const [k, v] of Object.entries(group)) checkEntry(`${name}.${k}`, v);
};
if (manifest.schema !== "pelican.ui_semantic_fixture.coverage" || manifest.version !== 3) fail("manifest header mismatch");
if (typeof manifest.description !== "string" || manifest.description.length > 2000) fail("manifest description must be a string of at most 2000 characters");
for (const k of Object.keys(manifest)) {
  if (!["schema", "version", "description", "enum_coverage", "invalid_class_coverage", "semantic_invariant_coverage"].includes(k)) fail(`manifest: unknown top-level key '${k}'`);
}
checkClosedGroup("invalid_class_coverage", manifest.invalid_class_coverage,
  coverageSchema.properties.invalid_class_coverage.required);
if (clauseRegistry.schema !== "pelican.ui_semantic_clauses" || clauseRegistry.version !== 1 || !Array.isArray(clauseRegistry.clauses)) fail("clause registry header mismatch");
const registryIds = clauseRegistry.clauses.map(c => c.id);
if (new Set(registryIds).size !== registryIds.length || registryIds.some(id => typeof id !== "string")) fail("clause registry contains invalid or duplicate ids");
checkClosedGroup("semantic_invariant_coverage", manifest.semantic_invariant_coverage, registryIds);
setEq("enum_coverage groups", Object.keys(manifest.enum_coverage ?? {}),
  coverageSchema.properties.enum_coverage.required);
for (const [g, m] of Object.entries(manifest.enum_coverage ?? {})) {
  if (typeof m !== "object" || m === null || Object.keys(m).length < 1) fail(`enum_coverage.${g}: entry map must not be empty`);
  for (const [k, v] of Object.entries(m)) checkEntry(`enum_coverage.${g}.${k}`, v);
}

// --- 2. fixture existence ---
const allEntries = [];
for (const m of Object.values(manifest.enum_coverage ?? {})) allEntries.push(...Object.values(m));
allEntries.push(...Object.values(manifest.invalid_class_coverage ?? {}));
allEntries.push(...Object.values(manifest.semantic_invariant_coverage ?? {}));
for (const e of allEntries) {
  if (e.fixture && !fs.existsSync(path.join(here, e.fixture))) fail(`referenced fixture does not exist: ${e.fixture}`);
}

// Referenced enum entries are executable witnesses: the named value must occur
// in the corresponding field (vocabulary classes match their prefix).
const fixtureCache = new Map();
const loadFixture = rel => {
  if (!fixtureCache.has(rel)) fixtureCache.set(rel, JSON.parse(fs.readFileSync(path.join(here, rel), "utf8")));
  return fixtureCache.get(rel);
};
const arrayHas = (fixture, arrayName, field, value) => (fixture[arrayName] ?? []).some(item => {
  if (field === "effects") return (item.effects ?? []).includes(value);
  if (field === "consumed") return (item.consumed ?? []).some(v => v.startsWith(`${value}:`));
  return item[field] === value;
});
const witnessHas = (group, value, fixture) => {
  if (group === "input_trace.kind") return arrayHas(fixture, "input_trace", "kind", value);
  if (group === "input_trace.button") return arrayHas(fixture, "input_trace", "button", value);
  if (group === "effects") return arrayHas(fixture, "input_trace", "effects", value);
  if (group === "consumed_vocabulary_class") return arrayHas(fixture, "input_trace", "consumed", value);
  if (group === "lifecycle.kind") return arrayHas(fixture, "lifecycle", "kind", value);
  if (group === "capture_cancel.reason") return (fixture.lifecycle ?? []).some(v => v.kind === "capture_cancel" && v.reason === value);
  if (group === "command_dropped.reason") return (fixture.lifecycle ?? []).some(v => v.kind === "command_dropped" && v.reason === value);
  if (group === "errors.phase") return arrayHas(fixture, "errors", "phase", value);
  if (group === "errors.code") return arrayHas(fixture, "errors", "code", value);
  if (group === "sampler") return arrayHas(fixture, "draw_runs", "sampler", value);
  return false;
};
for (const [group, entries] of Object.entries(manifest.enum_coverage ?? {})) {
  for (const [value, entry] of Object.entries(entries)) {
    if (entry.fixture && fs.existsSync(path.join(here, entry.fixture)) && !witnessHas(group, value, loadFixture(entry.fixture)))
      fail(`enum_coverage.${group}.${value}: referenced fixture does not contain the value`);
  }
}

// --- 3. enum extraction from fixture schema, exact equality ---
const defs = fixtureSchema.$defs;
const traceVariants = fixtureSchema.properties.input_trace.items.oneOf;
const kindValues = traceVariants.flatMap(v => {
  const k = v.properties.kind;
  return k.const ? [k.const] : k.enum;
});
const lifecycleVariants = fixtureSchema.properties.lifecycle.items.oneOf;
const lifecycleKinds = lifecycleVariants.flatMap(v => {
  const k = v.properties.kind;
  return k.const ? [k.const] : k.enum;
});
const captureCancel = lifecycleVariants.find(v => v.properties.kind.const === "capture_cancel");
const commandDropped = lifecycleVariants.find(v => v.properties.kind.const === "command_dropped");
const errorsItems = fixtureSchema.properties.errors.items;
const consumedClasses = defs.consumed_control.oneOf.map(o => {
  const m = /\^([a-z]+):/.exec(o.pattern);
  return m ? m[1] : "(unparsed)";
});

setEq("enum_coverage['input_trace.kind']", Object.keys(manifest.enum_coverage["input_trace.kind"]), kindValues);
setEq("enum_coverage['input_trace.button']", Object.keys(manifest.enum_coverage["input_trace.button"]), defs.button.enum);
setEq("enum_coverage['effects']", Object.keys(manifest.enum_coverage["effects"]), defs.effect_list.items.enum);
setEq("enum_coverage['consumed_vocabulary_class']", Object.keys(manifest.enum_coverage["consumed_vocabulary_class"]), consumedClasses);
setEq("enum_coverage['lifecycle.kind']", Object.keys(manifest.enum_coverage["lifecycle.kind"]), lifecycleKinds);
setEq("enum_coverage['capture_cancel.reason']", Object.keys(manifest.enum_coverage["capture_cancel.reason"]), captureCancel.properties.reason.enum);
setEq("enum_coverage['command_dropped.reason']", Object.keys(manifest.enum_coverage["command_dropped.reason"]), commandDropped.properties.reason.enum);
setEq("enum_coverage['errors.phase']", Object.keys(manifest.enum_coverage["errors.phase"]), errorsItems.properties.phase.enum);
setEq("enum_coverage['errors.code']", Object.keys(manifest.enum_coverage["errors.code"]), defs.error_code.enum);
setEq("enum_coverage['sampler']", Object.keys(manifest.enum_coverage["sampler"]), fixtureSchema.properties.draw_runs.items.properties.sampler.enum);

// --- 4. ui_key / ui_pad_control enum <-> pattern sync ---
const syncPattern = (name, prefix, enumValues) => {
  const branch = defs.consumed_control.oneOf.find(o => o.pattern.startsWith(`^${prefix}:`));
  if (!branch) return fail(`consumed_control: no ${prefix}: branch`);
  const m = /^\^[a-z]+:\((.*)\)\$$/.exec(branch.pattern);
  if (!m) return fail(`consumed_control ${prefix}: pattern not a plain alternation`);
  setEq(`${name} enum vs ${prefix}: pattern`, m[1].split("|"), enumValues);
};
syncPattern("$defs.ui_key", "key", defs.ui_key.enum);
syncPattern("$defs.ui_pad_control", "pad", defs.ui_pad_control.enum);

if (failures) {
  console.error(`\n${failures} failure(s)`);
  process.exit(1);
}
console.log("coverage gate: all checks passed");
