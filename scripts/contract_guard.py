#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QUGATE_H = ROOT / "QuGate.h"
HARNESS_CPP = ROOT / "contract_qugate.cpp"

CONSTANTS = [
    "QUGATE_INITIAL_MAX_GATES",
    "QUGATE_DEFAULT_CREATION_FEE",
    "QUGATE_DEFAULT_MIN_SEND",
    "QUGATE_FEE_ESCALATION_STEP",
    "QUGATE_DEFAULT_EXPIRY_EPOCHS",
]

MACRO_PATTERN = re.compile(
    r"^\s*(PUBLIC_FUNCTION_WITH_LOCALS|PUBLIC_PROCEDURE_WITH_LOCALS|PRIVATE_FUNCTION_WITH_LOCALS|PRIVATE_PROCEDURE_WITH_LOCALS|REGISTER_USER_FUNCTIONS_AND_PROCEDURES)\((\w*)\)",
    re.MULTILINE,
)
STRUCT_PATTERN = re.compile(r"^\s*struct\s+(\w+_locals)\s*\{", re.MULTILINE)
CONST_PATTERN = re.compile(r"constexpr\s+\w+\s+(\w+)\s*=\s*([^;]+);")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def extract_constants(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for name, value in CONST_PATTERN.findall(text):
        if name in CONSTANTS:
            values[name] = re.sub(r"\s+", " ", value.strip())
    return values


def next_macro_start(text: str, start: int) -> int:
    match = MACRO_PATTERN.search(text, start)
    return match.start() if match else len(text)


def extract_macro_blocks(text: str) -> list[tuple[str, str, str]]:
    matches = list(MACRO_PATTERN.finditer(text))
    blocks: list[tuple[str, str, str]] = []
    for i, match in enumerate(matches):
        macro_kind, name = match.group(1), match.group(2)
        if macro_kind == "REGISTER_USER_FUNCTIONS_AND_PROCEDURES":
            continue
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        blocks.append((macro_kind, name, text[match.start():end]))
    return blocks


def extract_struct_block(text: str, name: str) -> str:
    pattern = re.compile(rf"^\s*struct\s+{re.escape(name)}\s*\{{", re.MULTILINE)
    match = pattern.search(text)
    if not match:
        return ""
    i = match.end()
    depth = 1
    while i < len(text) and depth > 0:
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return text[match.start():i]


def find_private_procedure_calls_in_public_functions(text: str) -> list[str]:
    blocks = extract_macro_blocks(text)
    private_procedures = {name for kind, name, _ in blocks if kind == "PRIVATE_PROCEDURE_WITH_LOCALS"}
    problems: list[str] = []
    for kind, name, body in blocks:
        if kind != "PUBLIC_FUNCTION_WITH_LOCALS":
            continue
        for proc_name in sorted(private_procedures):
            if re.search(rf"\b{re.escape(proc_name)}\s*\(", body):
                problems.append(f"Public function `{name}` calls private procedure `{proc_name}`")
    return problems


def analyze_locals_hotspots(text: str) -> list[tuple[str, int, int]]:
    hotspots: list[tuple[str, int, int]] = []
    for struct_name in STRUCT_PATTERN.findall(text):
        block = extract_struct_block(text, struct_name)
        route_refs = len(re.findall(r"\brouteToGate_locals\b", block))
        process_refs = len(re.findall(r"\bprocess\w+_locals\b", block))
        if route_refs or process_refs >= 3:
            hotspots.append((struct_name, route_refs, process_refs))
    hotspots.sort(key=lambda item: (item[1], item[2], item[0]), reverse=True)
    return hotspots


def main() -> int:
    qugate_text = read_text(QUGATE_H)
    harness_text = read_text(HARNESS_CPP)

    errors: list[str] = []
    warnings: list[str] = []

    qugate_constants = extract_constants(qugate_text)
    harness_constants = extract_constants(harness_text)
    for name in CONSTANTS:
        qugate_value = qugate_constants.get(name)
        harness_value = harness_constants.get(name)
        if qugate_value is None:
            errors.append(f"Missing `{name}` in QuGate.h")
        elif harness_value is None:
            errors.append(f"Missing `{name}` in contract_qugate.cpp")
        elif qugate_value != harness_value:
            errors.append(
                f"Constant mismatch for `{name}`: QuGate.h has `{qugate_value}`, harness has `{harness_value}`"
            )

    errors.extend(find_private_procedure_calls_in_public_functions(qugate_text))

    hotspots = analyze_locals_hotspots(qugate_text)
    for struct_name, route_refs, process_refs in hotspots[:8]:
        warnings.append(
            f"Locals hotspot `{struct_name}` contains routeToGate/process locals "
            f"(routeToGate_locals={route_refs}, process*_locals={process_refs})"
        )

    if errors:
        print("contract_guard: FAILED")
        for error in errors:
            print(f"ERROR: {error}")
        if warnings:
            for warning in warnings:
                print(f"WARNING: {warning}")
        return 1

    print("contract_guard: OK")
    for name in CONSTANTS:
        print(f"OK: {name} matches")
    if warnings:
        for warning in warnings:
            print(f"WARNING: {warning}")
    else:
        print("OK: no obvious locals hotspots detected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
