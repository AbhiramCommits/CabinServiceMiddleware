#!/usr/bin/env python3
"""Deterministic + optional-LLM test generator for the cabin IDL.

Reads generated/cabin_schema.json and emits pinned GoogleTest suites plus the
JSON case file that drives them. The deterministic stage always runs and needs
no network. The optional LLM stage only runs with --llm and
ANTHROPIC_API_KEY; any failure there is a warning, never a build failure.

Usage: tools/gen_tests.py [--seed N] [--llm] [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import zlib
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = ROOT / "generated" / "cabin_schema.json"
OUT_DIR = ROOT / "tests" / "generated"
RESULTS_PATH = ROOT / "results" / "testgen.json"
GENERATOR_VERSION = "1.0.0"
DEFAULT_SEED = 20260101
DEFAULT_MODEL = "claude-sonnet-4-20250514"

INTEGER_RANGES = {
    "u8": (0, 255),
    "u16": (0, 65535),
    "u32": (0, 4294967295),
    "u64": (0, 18446744073709551615),
    "i32": (-2147483648, 2147483647),
}


def die(message: str) -> "NoReturn":
    print(f"gen_tests: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def load_schema() -> Dict[str, Any]:
    if not SCHEMA_PATH.exists():
        die("generated/cabin_schema.json is missing; run tools/idlc.py first")
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Wire encoding (mirrors generated cabin_serdes.cpp)
# ---------------------------------------------------------------------------


def encode_scalar(field: Dict[str, Any], value: Any) -> bytes:
    type_name = field["type"]
    if type_name == "u8":
        return struct.pack("<B", int(value) & 0xFF)
    if type_name == "u16":
        return struct.pack("<H", int(value) & 0xFFFF)
    if type_name == "u32":
        return struct.pack("<I", int(value) & 0xFFFFFFFF)
    if type_name == "u64":
        return struct.pack("<Q", int(value) & 0xFFFFFFFFFFFFFFFF)
    if type_name == "i32":
        return struct.pack("<i", int(value))
    if type_name == "f32":
        return struct.pack("<f", float(value))
    if type_name == "f64":
        return struct.pack("<d", float(value))
    if type_name == "bool":
        return struct.pack("<B", 1 if value else 0)
    if type_name == "string":
        raw = str(value).encode("utf-8")
        if len(raw) > 0xFFFF:
            raw = raw[:0xFFFF]
        return struct.pack("<H", len(raw)) + raw
    die(f"cannot encode scalar type {type_name}")
    raise AssertionError


def sample_value(schema: Dict[str, Any], field: Dict[str, Any]) -> Any:
    type_name = field["type"]
    if type_name == "string":
        limit = int(field.get("max_len") or 256)
        return "x" * min(4, limit)
    if type_name == "bool":
        return True
    if type_name.startswith("enum:"):
        return 0
    if type_name.startswith("struct:"):
        return None
    if type_name == "f32" or type_name == "f64":
        low = field.get("min")
        high = field.get("max")
        if low is not None and high is not None:
            return (float(low) + float(high)) / 2.0
        return 1.0
    low = field.get("min")
    high = field.get("max")
    if low is not None and high is not None:
        return int((int(low) + int(high)) // 2)
    if type_name in INTEGER_RANGES:
        return min(1, INTEGER_RANGES[type_name][1])
    return 0


def encode_struct(schema: Dict[str, Any], name: str,
                  overrides: Optional[Dict[str, Any]] = None) -> bytes:
    overrides = overrides or {}
    out = bytearray()
    for field in schema["structs"][name]["fields"]:
        field_name = field["name"]
        type_name = field["type"]
        value = overrides[field_name] if field_name in overrides else sample_value(schema, field)
        if type_name.startswith("struct:"):
            nested = encode_struct(schema, type_name.split(":", 1)[1])
            out += struct.pack("<H", len(nested))
            out += nested
        elif type_name.startswith("enum:"):
            enum_name = type_name.split(":", 1)[1]
            index = int(value)
            if isinstance(value, str):
                index = schema["enums"][enum_name]["values_by_name"].get(value, 0)
            out += struct.pack("<B", index & 0xFF)
        else:
            out += encode_scalar(field, value)
    return bytes(out)


def frame_bytes(schema: Dict[str, Any], service_id: int, method_id: int,
                payload: bytes, major: Optional[int] = None,
                payload_length: Optional[int] = None) -> bytes:
    version = schema["version"]
    header = bytearray()
    header += b"CBN1"
    header += struct.pack("<B", version["major"] if major is None else major)
    header += struct.pack("<B", version["minor"])
    header += struct.pack("<B", 1)  # REQUEST
    header += struct.pack("<B", 0)  # flags
    header += struct.pack("<H", service_id)
    header += struct.pack("<H", method_id)
    header += struct.pack("<I", 1)  # seq
    header += struct.pack("<Q", 0x8000000000000001)
    length = len(payload) if payload_length is None else payload_length
    header += struct.pack("<I", length)
    if payload_length is None:
        body = bytes(header) + payload
        return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    return bytes(header)


# ---------------------------------------------------------------------------
# Deterministic case generation
# ---------------------------------------------------------------------------


def add_case(cases: List[Dict[str, Any]], seen: set, case: Dict[str, Any]) -> bool:
    key = json.dumps(
        {k: v for k, v in case.items() if k not in ("id", "rationale")},
        sort_keys=True,
        default=str,
    )
    if key in seen:
        return False
    seen.add(key)
    cases.append(case)
    return True


def generate_field_cases(schema: Dict[str, Any], cases: List[Dict[str, Any]],
                         seen: set) -> int:
    count = 0
    for struct_name in sorted(schema["structs"]):
        for field in schema["structs"][struct_name]["fields"]:
            name = field["name"]
            type_name = field["type"]
            if type_name.startswith("struct:"):
                continue
            base = {
                "kind": "serdes",
                "struct": struct_name,
                "field": name,
                "origin": "schema",
            }

            def emit(label: str, expect: str, value: Any = None,
                     special: Optional[str] = None, text: Optional[str] = None,
                     rationale: str = "") -> None:
                nonlocal count
                case = dict(base)
                case["id"] = f"serdes:{struct_name}.{name}:{label}"
                case["expect"] = expect
                case["rationale"] = rationale or label
                if value is not None:
                    case["value"] = value
                if special is not None:
                    case["special"] = special
                if text is not None:
                    case["text"] = text
                if add_case(cases, seen, case):
                    count += 1

            if type_name == "string":
                limit = int(field.get("max_len") or 256)
                emit("empty", "valid", text="", rationale="empty string is valid")
                emit("max_len", "valid", text="s" * limit,
                     rationale="exactly max_len bytes")
                emit("max_len_plus_one", "rejected", text="s" * (limit + 1),
                     rationale="one byte beyond max_len")
                continue
            if type_name == "bool":
                emit("true", "valid", value=1)
                emit("false", "valid", value=0)
                continue
            if type_name.startswith("enum:"):
                enum_name = type_name.split(":", 1)[1]
                values = schema["enums"][enum_name]["values"]
                emit("min", "valid", value=0)
                emit("max", "valid", value=len(values) - 1)
                emit("mid", "valid", value=len(values) // 2)
                emit("max_plus_one", "rejected", value=len(values))
                continue

            low = field.get("min")
            high = field.get("max")
            if low is None or high is None:
                continue
            is_float = type_name in ("f32", "f64")
            emit("min", "valid", value=low, rationale="lower bound accepted")
            emit("max", "valid", value=high, rationale="upper bound accepted")
            if low != high:
                midpoint = (float(low) + float(high)) / 2.0
                emit("mid", "valid", value=midpoint,
                     rationale="equivalence-class midpoint")
            if float(low) > 0.0:
                emit("min_minus_one", "rejected",
                     value=float(low) - (1.0 if is_float else 1),
                     rationale="just below the lower bound")
            if is_float:
                emit("max_plus_one", "rejected", value=float(high) + 1.0,
                     rationale="just above the upper bound")
                emit("nan", "rejected", special="nan",
                     rationale="NaN must be rejected")
                emit("plus_inf", "rejected", special="inf",
                     rationale="+Inf must be rejected")
                emit("minus_inf", "rejected", special="-inf",
                     rationale="-Inf must be rejected")
            else:
                emit("max_plus_one", "rejected", value=int(high) + 1,
                     rationale="just above the upper bound")
            if float(low) <= 0.0 <= float(high):
                emit("zero", "valid", value=0)
    return count


def scaled_raw(signal: Dict[str, Any], value: float) -> Optional[int]:
    can = signal["can"]
    bits = int(can["bit_length"])
    if can["encoding"] == "f32":
        return struct.unpack("<I", struct.pack("<f", float(value)))[0]
    raw = round((float(value) - float(can["offset"])) / float(can["scale"]))
    if raw < 0 or raw > (1 << bits) - 1:
        return None
    return int(raw)


def generate_signal_cases(schema: Dict[str, Any], cases: List[Dict[str, Any]],
                          seen: set) -> int:
    count = 0
    for signal in schema["signals"]:
        name = signal["name"]
        low = float(signal["min"])
        high = float(signal["max"])
        is_float = signal["type"] in ("f32", "f64")
        base = {"kind": "signal", "signal": name, "origin": "schema"}

        def emit(label: str, raw_bits: Optional[int], expect: str,
                 expected_value: float, rationale: str) -> None:
            nonlocal count
            if raw_bits is None:
                return
            case = dict(base)
            case["id"] = f"signal:{name}:{label}"
            case["raw_bits"] = int(raw_bits)
            case["expect"] = expect
            case["expected_value"] = expected_value
            case["rationale"] = rationale
            if add_case(cases, seen, case):
                count += 1

        emit("min", scaled_raw(signal, low), "valid", low,
             "lower bound decodes without clamping")
        if high != low:
            midpoint = (low + high) / 2.0
            if not is_float:
                midpoint = float(math.floor(midpoint))
            raw = scaled_raw(signal, midpoint)
            expected_mid = (
                midpoint
                if is_float or raw is None
                else raw * float(signal["can"]["scale"]) + float(signal["can"]["offset"])
            )
            emit("mid", raw, "valid", expected_mid,
                 "equivalence-class midpoint decodes without clamping")
        emit("max", scaled_raw(signal, high), "valid", high,
             "upper bound decodes without clamping")
        if low > 0:
            below = scaled_raw(signal, low - 1.0)
            emit("below_min", below, "clamped", low,
                 "raw value below the range clamps to min")
        above = scaled_raw(signal, high + 1.0)
        if above is not None:
            emit("above_max", above, "clamped", high,
                 "raw value above the range clamps to max")
        if is_float:
            emit("nan", 0x7FC00000, "invalid", low,
                 "NaN must not propagate (safe fallback + invalid quality)")
            emit("plus_inf", 0x7F800000, "clamped", high,
                 "+Inf clamps to max")
            emit("minus_inf", 0xFF800000, "clamped", low,
                 "-Inf clamps to min")
        if signal["enum"] is not None:
            values = schema["enums"][signal["enum"]]["values"]
            emit("invalid_enum", len(values), "clamped", float(len(values) - 1),
                 "out-of-range enum clamps to the last declared value")
    return count


def generate_api_cases(schema: Dict[str, Any], cases: List[Dict[str, Any]],
                       seen: set) -> int:
    count = 0
    for service in schema["services"]:
        for method in service["methods"]:
            request = schema["structs"][method["request"]]
            fields = request["fields"]
            service_id = service["id"]
            method_id = method["id"]
            base = {
                "kind": "api",
                "service": service["name"],
                "method": method["name"],
                "origin": "schema",
            }

            def emit(label: str, payload: bytes, expect: str,
                     error: Optional[str], rationale: str) -> None:
                nonlocal count
                case = dict(base)
                case["id"] = f"api:{service['name']}.{method['name']}:{label}"
                case["payload_hex"] = payload.hex()
                case["expect"] = expect
                if error:
                    case["error"] = error
                case["rationale"] = rationale
                if add_case(cases, seen, case):
                    count += 1

            valid = encode_struct(schema, method["request"])
            if fields:
                emit("valid", valid, "accepted", None,
                     "baseline request decodes")
                if len(valid) > 0:
                    emit("missing_field", valid[:-1], "rejected", "TRUNCATED",
                         "last field is cut off")
                emit("missing_all", b"", "rejected", "TRUNCATED",
                     "empty payload for a non-empty request")
                emit("extra_field", valid + b"\xde\xad\xbe\xef", "accepted", None,
                     "trailing bytes are ignored for forward compatibility")

                wrong: Optional[bytes] = None
                for field in fields:
                    type_name = field["type"]
                    if type_name.startswith("struct:"):
                        continue
                    if type_name == "string":
                        wrong = encode_struct(
                            schema, method["request"],
                            {field["name"]: "z" * (int(field.get("max_len") or 256) + 1)})
                        break
                    if type_name.startswith("enum:"):
                        enum_name = type_name.split(":", 1)[1]
                        wrong = encode_struct(
                            schema, method["request"],
                            {field["name"]: len(schema["enums"][enum_name]["values"])})
                        break
                    if field.get("min") is not None and field.get("max") is not None:
                        wrong = encode_struct(
                            schema, method["request"],
                            {field["name"]: float(field["max"]) + 1.0})
                        break
                if wrong is not None:
                    emit("wrong_type_or_range", wrong, "rejected", "OUT_OF_RANGE",
                         "a field is outside its declared range")

            oversized = frame_bytes(schema, service_id, method_id, b"",
                                    payload_length=16 * 1024 * 1024 + 1)
            frame_case = {
                "kind": "frame",
                "id": f"frame:{service['name']}.{method['name']}:oversized_payload",
                "service": service["name"],
                "method": method["name"],
                "raw_hex": oversized.hex(),
                "expect": "rejected",
                "error": "INVALID_ARGUMENT",
                "origin": "schema",
                "rationale": "payload_len above the 16 MiB cap is rejected before allocation",
            }
            if add_case(cases, seen, frame_case):
                count += 1

            good_frame = frame_bytes(schema, service_id, method_id, valid)
            bad_major = frame_bytes(schema, service_id, method_id, valid, major=2)
            for label, raw, expect, error, rationale in [
                ("bad_major_version", bad_major, "rejected", "VERSION_MISMATCH",
                 "mismatched IDL major is rejected"),
                ("truncated_frame", good_frame[:-2], "rejected", "TRUNCATED",
                 "truncated frame is rejected before payload decode"),
            ]:
                case = {
                    "kind": "frame",
                    "id": f"frame:{service['name']}.{method['name']}:{label}",
                    "service": service["name"],
                    "method": method["name"],
                    "raw_hex": raw.hex(),
                    "expect": expect,
                    "error": error,
                    "origin": "schema",
                    "rationale": rationale,
                }
                if add_case(cases, seen, case):
                    count += 1
    return count


# ---------------------------------------------------------------------------
# Optional LLM stage
# ---------------------------------------------------------------------------


def llm_generate(schema: Dict[str, Any], seed: int, spec: str,
                 existing: set) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    stats: Dict[str, Any] = {
        "status": "disabled",
        "model": "none",
        "proposed": 0,
        "accepted": 0,
        "rejected": 0,
    }
    if spec != "llm":
        return [], stats
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        stats["status"] = "skipped_no_key"
        return [], stats
    model = os.environ.get("CABIN_LLM_MODEL", DEFAULT_MODEL)
    stats["model"] = model
    stats["status"] = "ok"

    prompt = (
        "You are generating adversarial test cases for a car cabin middleware "
        "IDL. Return ONLY a JSON array. Each element must be an object with "
        "keys: kind (\"combo\"), struct (one of the struct names below), "
        "overrides (an object mapping field names to numeric values), "
        "expect (\"valid\" or \"rejected\"), rationale (short string). "
        "Propose physically or semantically interesting combinations, e.g. "
        "seat_recline_deg=60 with seat_position_mm=0, or HVAC fan at max with "
        "setpoint at the lower bound. Do not invent fields. Schema:\n"
        + json.dumps(
            {
                "structs": {
                    name: [
                        {
                            "name": field["name"],
                            "type": field["type"],
                            "min": field["min"],
                            "max": field["max"],
                        }
                        for field in value["fields"]
                        if not field["type"].startswith("struct:")
                    ]
                    for name, value in schema["structs"].items()
                },
                "seed": seed,
            },
            sort_keys=True,
        )
    )
    body = json.dumps(
        {
            "model": model,
            "max_tokens": 2000,
            "temperature": 0.2,
            "messages": [{"role": "user", "content": prompt}],
        }
    ).encode()
    request = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "x-api-key": key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.loads(response.read())
        text = "".join(
            block.get("text", "")
            for block in payload.get("content", [])
            if isinstance(block, dict)
        )
        text = text.strip()
        if text.startswith("```"):
            text = re.sub(r"^```[a-zA-Z]*\n?", "", text)
            text = re.sub(r"\n?```$", "", text)
        proposed = json.loads(text)
        if not isinstance(proposed, list):
            raise ValueError("LLM output is not a JSON array")
    except (urllib.error.URLError, TimeoutError, ValueError, KeyError) as error:
        stats["status"] = "error"
        stats["error"] = str(error)
        print(f"gen_tests: warning: LLM stage skipped: {error}", file=sys.stderr)
        return [], stats

    stats["proposed"] = len(proposed)
    accepted: List[Dict[str, Any]] = []
    for index, item in enumerate(proposed):
        try:
            if not isinstance(item, dict) or item.get("kind") != "combo":
                stats["rejected"] += 1
                continue
            struct_name = item["struct"]
            overrides = item["overrides"]
            expect = item["expect"]
            rationale = item["rationale"]
            if (struct_name not in schema["structs"] or
                    not isinstance(overrides, dict) or not overrides or
                    expect not in ("valid", "rejected") or
                    not isinstance(rationale, str) or not rationale):
                stats["rejected"] += 1
                continue
            fields = {f["name"]: f for f in schema["structs"][struct_name]["fields"]}
            clean: Dict[str, float] = {}
            for field_name, value in overrides.items():
                field = fields.get(field_name)
                if field is None or not isinstance(value, (int, float)) or isinstance(value, bool):
                    raise ValueError
                type_name = field["type"]
                if type_name.startswith(("struct:", "string", "bool")):
                    raise ValueError
                if field.get("min") is not None and not (
                        float(field["min"]) <= float(value) <= float(field["max"])):
                    if expect == "valid":
                        raise ValueError
                clean[field_name] = float(value)
            if not clean:
                raise ValueError
            case = {
                "kind": "combo",
                "id": f"combo:{struct_name}:{index}",
                "struct": struct_name,
                "overrides": clean,
                "expect": expect,
                "origin": "llm",
                "rationale": rationale[:200],
            }
            key_text = json.dumps(
                {k: v for k, v in case.items() if k not in ("id", "rationale")},
                sort_keys=True,
            )
            if key_text in existing:
                stats["rejected"] += 1
                continue
            existing.add(key_text)
            accepted.append(case)
            stats["accepted"] += 1
        except (KeyError, TypeError, ValueError):
            stats["rejected"] += 1
    return accepted, stats


# ---------------------------------------------------------------------------
# C++ emission
# ---------------------------------------------------------------------------


def field_order(schema: Dict[str, Any]) -> List[str]:
    ordered: List[str] = []
    remaining = set(schema["structs"])
    deps = {
        name: {
            f["type"].split(":", 1)[1]
            for f in value["fields"]
            if f["type"].startswith("struct:")
        }
        for name, value in schema["structs"].items()
    }
    while remaining:
        ready = sorted(n for n in remaining if deps[n] <= set(ordered))
        if not ready:
            die("cyclic struct dependency")
        ordered.extend(ready)
        remaining -= set(ready)
    return ordered


def cursor_type(schema: Dict[str, Any], field: Dict[str, Any]) -> str:
    if field["type"] == "string":
        return "std::string"
    if field["type"] == "bool":
        return "bool"
    return field["cpp_type"]


def emit_case_loader() -> str:
    return """// GENERATED by tools/gen_tests.py - do not edit
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cabin/json.hpp"

struct GeneratedCase {
  std::string id;
  std::string kind;
  std::string struct_name;
  std::string field;
  std::string expect;
  std::string special;
  std::string text;
  std::string signal;
  std::string service;
  std::string method;
  std::string error;
  std::string payload_hex;
  std::string raw_hex;
  std::string origin;
  std::string rationale;
  double value{0.0};
  bool has_value{false};
  std::uint64_t raw_bits{0};
  std::map<std::string, double> overrides;
};

inline std::vector<GeneratedCase> load_generated_cases(const std::string& filter) {
  std::vector<GeneratedCase> cases;
  std::ifstream input(CABIN_CASES_PATH);
  if (!input.is_open()) return cases;
  std::ostringstream buffer;
  buffer << input.rdbuf();
  auto parsed = cabin::parse_json(buffer.str());
  if (!parsed) return cases;
  const cabin::JsonValue* list = parsed.value().find("cases");
  if (list == nullptr || !list->is_array()) return cases;
  for (std::size_t index = 0; index < list->size(); ++index) {
    const cabin::JsonValue& item = *list->at(index);
    GeneratedCase item_case;
    item_case.id = item.get_string("id");
    item_case.kind = item.get_string("kind");
    item_case.struct_name = item.get_string("struct");
    item_case.field = item.get_string("field");
    item_case.expect = item.get_string("expect");
    item_case.special = item.get_string("special");
    item_case.text = item.get_string("text");
    item_case.signal = item.get_string("signal");
    item_case.service = item.get_string("service");
    item_case.method = item.get_string("method");
    item_case.error = item.get_string("error");
    item_case.payload_hex = item.get_string("payload_hex");
    item_case.raw_hex = item.get_string("raw_hex");
    item_case.origin = item.get_string("origin");
    item_case.rationale = item.get_string("rationale");
    const cabin::JsonValue* value = item.find("value");
    if (value == nullptr) value = item.find("expected_value");
    if (value != nullptr && value->is_number()) {
      item_case.value = value->number_value();
      item_case.has_value = true;
    }
    const cabin::JsonValue* raw_bits = item.find("raw_bits");
    if (raw_bits != nullptr && raw_bits->is_number()) {
      item_case.raw_bits = static_cast<std::uint64_t>(raw_bits->number_value());
    }
    const cabin::JsonValue* overrides = item.find("overrides");
    if (overrides != nullptr && overrides->is_object()) {
      for (std::size_t entry = 0; entry < overrides->object_size(); ++entry) {
        const cabin::JsonValue& number = overrides->value_at(entry);
        if (number.is_number()) {
          item_case.overrides[overrides->key_at(entry)] = number.number_value();
        }
      }
    }
    const bool include =
        item_case.kind == filter ||
        (filter == "serdes" && item_case.kind == "combo");
    if (include) cases.push_back(item_case);
  }
  return cases;
}
"""


def emit_serdes_test(schema: Dict[str, Any]) -> str:
    out: List[str] = []
    out.append("""// GENERATED by tools/gen_tests.py - do not edit
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "cabin/signals/codec.hpp"
#include "cabin_can_map.hpp"
#include "cabin_serdes.hpp"
#include "case_loader.hpp"

using namespace cabin;

namespace {

double case_value(const GeneratedCase& test_case) {
  if (test_case.special == "nan") return std::numeric_limits<double>::quiet_NaN();
  if (test_case.special == "inf") return std::numeric_limits<double>::infinity();
  if (test_case.special == "-inf") return -std::numeric_limits<double>::infinity();
  return test_case.value;
}

""")
    out.append("bool apply_field_case(const GeneratedCase& test_case, std::string* error) {\n")
    out.append("  const double value = case_value(test_case);\n")
    for struct_name in field_order(schema):
        out.append(f'  if (test_case.struct_name == "{struct_name}") {{\n')
        out.append(f"    gen::{struct_name} record{{}};\n")
        for field in schema["structs"][struct_name]["fields"]:
            name = field["name"]
            type_name = field["type"]
            if type_name.startswith("struct:"):
                continue
            out.append(f'    if (test_case.field == "{name}") {{\n')
            if type_name == "string":
                out.append("      record." + name + " = test_case.text;\n")
            elif type_name == "bool":
                out.append(f"      record.{name} = value != 0.0;\n")
            elif type_name.startswith("enum:"):
                out.append(f"      record.{name} = static_cast<gen::{field['cpp_type']}>("
                           f"static_cast<std::uint8_t>(value));\n")
            elif type_name == "f32":
                out.append(f"      record.{name} = static_cast<float>(value);\n")
            elif type_name == "f64":
                out.append(f"      record.{name} = value;\n")
            elif type_name == "bool":
                out.append(f"      record.{name} = value != 0.0;\n")
            else:
                out.append(f"      record.{name} = static_cast<{field['cpp_type']}>(value);\n")
            out.append("    } else\n")
        out.append(
            f'    {{ *error = "unknown field for {struct_name}"; return false; }}\n'
        )
        out.append(f"    auto status = gen::validate(record);\n")
        out.append(f"    auto encoded = gen::encode(record);\n")
        out.append(f"    if (test_case.expect == \"valid\" && (!status.ok() || !encoded.ok())) {{\n")
        out.append(f"      *error = status.ok() ? encoded.error().message : status.error().message;\n")
        out.append(f"      return false;\n    }}\n")
        out.append(f"    if (test_case.expect == \"rejected\") {{\n")
        out.append(f"      if (status.ok() || encoded.ok()) {{ *error = \"expected rejection\"; return false; }}\n")
        out.append(f"    }}\n")
        out.append(f"    return true;\n  }}\n")
    out.append('  *error = "unknown struct";\n  return false;\n}\n\n')

    out.append("bool apply_combo_case(const GeneratedCase& test_case, std::string* error) {\n")
    for struct_name in field_order(schema):
        out.append(f'  if (test_case.struct_name == "{struct_name}") {{\n')
        out.append(f"    gen::{struct_name} record{{}};\n")
        out.append("    for (const auto& entry : test_case.overrides) {\n")
        out.append("      (void)entry;\n")
        for field in schema["structs"][struct_name]["fields"]:
            name = field["name"]
            type_name = field["type"]
            if type_name.startswith("struct:") or type_name in ("string", "bool"):
                continue
            out.append(f'      if (entry.first == "{name}") {{\n')
            if type_name.startswith("enum:"):
                out.append(f"        record.{name} = static_cast<gen::{field['cpp_type']}>("
                           f"static_cast<std::uint8_t>(entry.second));\n")
            elif type_name == "f32":
                out.append(f"        record.{name} = static_cast<float>(entry.second);\n")
            elif type_name == "f64":
                out.append(f"        record.{name} = entry.second;\n")
            else:
                out.append(f"        record.{name} = static_cast<{field['cpp_type']}>(entry.second);\n")
            out.append("        continue;\n      }\n")
        out.append('      *error = "unknown combo field"; return false;\n')
        out.append("    }\n")
        out.append(f"    auto status = gen::validate(record);\n")
        out.append(f"    auto encoded = gen::encode(record);\n")
        out.append(f"    if (test_case.expect == \"valid\" && (!status.ok() || !encoded.ok())) {{\n")
        out.append(f"      *error = status.ok() ? encoded.error().message : status.error().message;\n")
        out.append(f"      return false;\n    }}\n")
        out.append(f"    if (test_case.expect == \"rejected\") {{\n")
        out.append(f"      if (status.ok() || encoded.ok()) {{ *error = \"expected rejection\"; return false; }}\n")
        out.append(f"    }}\n")
        out.append(f"    return true;\n  }}\n")
    out.append('  *error = "unknown combo struct";\n  return false;\n}\n\n')

    out.append("""}  // namespace

class GeneratedSerdes : public ::testing::TestWithParam<GeneratedCase> {};
class GeneratedSignals : public ::testing::TestWithParam<GeneratedCase> {};

TEST_P(GeneratedSerdes, MatchesPinnedExpectation) {
  const GeneratedCase& test_case = GetParam();
  std::string error;
  if (test_case.kind == "combo") {
    EXPECT_TRUE(apply_combo_case(test_case, &error)) << test_case.rationale << ": " << error;
  } else {
    EXPECT_TRUE(apply_field_case(test_case, &error)) << test_case.rationale << ": " << error;
  }
}

TEST_P(GeneratedSignals, MatchesPinnedExpectation) {
  const GeneratedCase& test_case = GetParam();
  const signals::SignalMeta* meta = signals::find_signal(test_case.signal);
  ASSERT_NE(meta, nullptr) << test_case.signal;
  signals::SignalCodec codec;
  auto decoded = codec.decode_bits(meta->id, test_case.raw_bits);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  const auto& signal = decoded.value();
  if (test_case.expect == "valid") {
    EXPECT_EQ(signal.quality, signals::SignalQuality::kOk) << test_case.rationale;
  } else if (test_case.expect == "clamped") {
    EXPECT_EQ(signal.quality, signals::SignalQuality::kClamped) << test_case.rationale;
  } else if (test_case.expect == "invalid") {
    EXPECT_EQ(signal.quality, signals::SignalQuality::kInvalid) << test_case.rationale;
  }
  const auto expected = test_case.value;
  if (std::isfinite(expected)) {
    EXPECT_NEAR(signal.value, expected, 1e-3) << test_case.rationale;
  }
}

INSTANTIATE_TEST_SUITE_P(Generated, GeneratedSerdes,
                         ::testing::ValuesIn(load_generated_cases("serdes")));
INSTANTIATE_TEST_SUITE_P(Generated, GeneratedSignals,
                         ::testing::ValuesIn(load_generated_cases("signal")));
""")
    return "".join(out)


def emit_api_test(schema: Dict[str, Any]) -> str:
    out: List[str] = []
    out.append("""// GENERATED by tools/gen_tests.py - do not edit
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cabin/frame.hpp"
#include "cabin_serdes.hpp"
#include "case_loader.hpp"

using namespace cabin;

namespace {

Bytes from_hex(const std::string& hex) {
  Bytes bytes;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    const int high = digit(hex[index]);
    const int low = digit(hex[index + 1]);
    if (high < 0 || low < 0) break;
    bytes.append_u8(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

Result<void> decode_request(const std::string& service, const std::string& method,
                            const Bytes& payload) {
""")
    for service in schema["services"]:
        for method in service["methods"]:
            request = method["request"]
            out.append(
                f'  if (service == "{service["name"]}" && method == "{method["name"]}") {{\n'
            )
            out.append(f"    auto value = gen::decode<gen::{request}>(payload);\n")
            out.append("    if (!value) return value.error();\n")
            out.append("    return ok_status();\n  }\n")
    out.append('  return make_error(ErrorCode::kNotFound, "unknown service/method");\n')
    out.append("}\n\n}  // namespace\n\n")
    out.append("""class GeneratedApi : public ::testing::TestWithParam<GeneratedCase> {};

TEST_P(GeneratedApi, MatchesPinnedExpectation) {
  const GeneratedCase& test_case = GetParam();
  if (test_case.kind == "frame") {
    const Bytes raw = from_hex(test_case.raw_hex);
    auto decoded = decode_frame(raw.data(), raw.size(), nullptr);
    if (test_case.expect == "rejected") {
      EXPECT_FALSE(decoded.ok()) << test_case.rationale;
      if (!test_case.error.empty()) {
        EXPECT_STREQ(error_code_name(decoded.code()), test_case.error.c_str())
            << test_case.rationale;
      }
    } else {
      EXPECT_TRUE(decoded.ok()) << test_case.rationale;
    }
    return;
  }
  const Bytes payload = from_hex(test_case.payload_hex);
  auto result = decode_request(test_case.service, test_case.method, payload);
  if (test_case.expect == "accepted") {
    EXPECT_TRUE(result.ok()) << test_case.rationale << ": " << result.error().message;
  } else {
    EXPECT_FALSE(result.ok()) << test_case.rationale;
    if (!test_case.error.empty()) {
      EXPECT_STREQ(error_code_name(result.code()), test_case.error.c_str())
          << test_case.rationale;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Generated, GeneratedApi,
                         ::testing::ValuesIn(load_generated_cases("api")));
""")
    return "".join(out)


def corpus_files(schema: Dict[str, Any], cases: List[Dict[str, Any]],
                 root: Path) -> Dict[Path, bytes]:
    files: Dict[Path, bytes] = {}
    signals_by_name = {signal["name"]: signal for signal in schema["signals"]}
    for case in cases:
        safe = re.sub(r"[^A-Za-z0-9_.-]", "_", case["id"])
        if case["kind"] == "frame":
            raw = bytes.fromhex(case["raw_hex"])
            files[root / "fuzz" / "corpus" / "frame" / f"{safe}.bin"] = raw
        elif case["kind"] == "signal":
            signal = signals_by_name.get(case["signal"])
            if signal is None:
                continue
            bits = int(case["raw_bits"])
            data = bytearray(8)
            for index in range(int(signal["can"]["bit_length"])):
                if (bits >> index) & 1:
                    bit = int(signal["can"]["bit_offset"]) + index
                    data[bit // 8] |= 1 << (bit % 8)
            record = struct.pack("<I", int(signal["can"]["frame_id"]))
            record += struct.pack("<B", 8)
            record += bytes(data)
            files[root / "fuzz" / "corpus" / "signal" / f"{safe}.bin"] = record
    return files


def emit_provenance(seed: int, stats: Dict[str, Any], llm: Dict[str, Any],
                    schema_hash: str) -> str:
    return f"""# Generated test provenance

- Generator: `tools/gen_tests.py` version **{GENERATOR_VERSION}**
- Deterministic seed: **{seed}**
- Schema SHA-256: `{schema_hash}`
- LLM model: **{llm["model"] if llm["status"] == "ok" else "none"}**
- LLM stage status: **{llm["status"]}**
- Cases accepted: **{stats["cases_total"]}**
  - schema origin: {stats["schema_cases"]}
  - llm origin: {stats["llm_cases"]}
  - expected valid: {stats["expected_valid"]}
  - expected rejected: {stats["expected_rejected"]}
- LLM proposals: {llm["proposed"]} (accepted {llm["accepted"]}, rejected {llm["rejected"]})

Regenerate with `python3 tools/gen_tests.py`. CI runs
`python3 tools/gen_tests.py --check` and fails on drift, so every generated
case has been reviewed and committed.
"""


def build_cases(schema: Dict[str, Any], seed: int, llm_mode: str) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    cases: List[Dict[str, Any]] = []
    seen: set = set()
    generate_field_cases(schema, cases, seen)
    generate_signal_cases(schema, cases, seen)
    generate_api_cases(schema, cases, seen)
    llm_cases, llm_stats = llm_generate(schema, seed, llm_mode, seen)
    cases.extend(llm_cases)
    cases.sort(key=lambda item: item["id"])
    expected_valid = sum(
        1 for item in cases if item["expect"] in ("valid", "accepted")
    )
    expected_rejected = sum(
        1 for item in cases if item["expect"] in ("rejected", "clamped", "invalid")
    )
    schema_hash = hashlib.sha256(SCHEMA_PATH.read_bytes()).hexdigest()
    stats = {
        "generator": "tools/gen_tests.py",
        "version": GENERATOR_VERSION,
        "seed": seed,
        "schema_sha256": schema_hash,
        "cases_total": len(cases),
        "schema_cases": sum(1 for item in cases if item["origin"] == "schema"),
        "llm_cases": sum(1 for item in cases if item["origin"] == "llm"),
        "expected_valid": expected_valid,
        "expected_rejected": expected_rejected,
        "llm_proposed": llm_stats["proposed"],
        "llm_accepted": llm_stats["accepted"],
        "llm_rejected": llm_stats["rejected"],
        "llm_status": llm_stats["status"],
    }
    return {"seed": seed, "counts": stats, "cases": cases}, llm_stats


def generate_tree(schema: Dict[str, Any], seed: int, llm_mode: str,
                  root: Path) -> Dict[Path, Union[str, bytes]]:
    document, llm_stats = build_cases(schema, seed, llm_mode)
    stats = document["counts"]
    out_dir = root / "tests" / "generated"
    files: Dict[Path, Union[str, bytes]] = {
        out_dir / "cases.json": json.dumps(document, indent=2, sort_keys=True) + "\n",
        out_dir / "case_loader.hpp": emit_case_loader(),
        out_dir / "test_generated_serdes.cpp": emit_serdes_test(schema),
        out_dir / "test_generated_api.cpp": emit_api_test(schema),
        out_dir / "PROVENANCE.md": emit_provenance(
            seed, stats, llm_stats, stats["schema_sha256"]
        ),
        root / "results" / "testgen.json": json.dumps(stats, indent=2, sort_keys=True) + "\n",
    }
    files.update(corpus_files(schema, document["cases"], root))
    return files


def main(argv: Iterable[str]) -> int:
    parser = argparse.ArgumentParser(description="Cabin test generator")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--llm", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(list(argv))
    schema = load_schema()
    llm_mode = "llm" if args.llm else "deterministic"

    if args.check:
        with tempfile.TemporaryDirectory(prefix="cabintest-check-") as temporary:
            root = Path(temporary)
            files = generate_tree(schema, args.seed, llm_mode, root)
            problems: List[str] = []
            for path, content in files.items():
                relative = path.relative_to(root)
                target = ROOT / relative
                if not target.exists():
                    problems.append(f"missing {relative}")
                elif isinstance(content, bytes):
                    if target.read_bytes() != content:
                        problems.append(f"drift in {relative}")
                elif target.read_text(encoding="utf-8") != content:
                    problems.append(f"drift in {relative}")
        if problems:
            for problem in problems:
                print(f"gen_tests --check: {problem}", file=sys.stderr)
            return 1
        print("gen_tests --check: generated tests are up to date")
        return 0

    files = generate_tree(schema, args.seed, llm_mode, ROOT)
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
    stats = json.loads((ROOT / "results" / "testgen.json").read_text())
    print(
        f"gen_tests: wrote {stats['cases_total']} cases "
        f"({stats['schema_cases']} schema, {stats['llm_cases']} llm), "
        f"valid={stats['expected_valid']} rejected={stats['expected_rejected']}"
    )
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main(sys.argv[1:]))
