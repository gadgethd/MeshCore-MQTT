#!/usr/bin/env python3
"""Structurally compare ukmesh payloads against the frozen C6 fixtures.

Ignores values (timestamps, counters, hex blobs); flags added/removed keys and
type-class changes, i.e. quiet changes to the payload contract.

Usage:
  scripts/check-ukmesh-payload-fixtures.py NEW.json
  scripts/check-ukmesh-payload-fixtures.py --capture capture.jsonl
Options:
  --family status|packet|neighbors  (default: auto-detect for single files)
  --fixture-dir DIR                 (default: test/fixtures/ukmesh)
Exit code: 0 = no structural changes, 1 = differences found, 2 = usage error.
"""

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURES = REPO / "test" / "fixtures" / "ukmesh"
FIXTURE_FILES = {
    "status": "status-online.sample.json",
    "packet": "packet-tx.sample.json",
    "neighbors": "neighbors.sample.json",
}


def type_class(v):
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, dict):
        return "object"
    if isinstance(v, list):
        return "array"
    if v is None:
        return "null"
    return type(v).__name__


def structure(obj, prefix=""):
    out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            key = f"{prefix}{k}"
            out[key] = type_class(v)
            if isinstance(v, dict):
                out.update(structure(v, key + "."))
    return out


def detect_family(obj):
    if not isinstance(obj, dict):
        return None
    if obj.get("type") == "PACKET" and "raw" in obj:
        return "packet"
    if "status" in obj and "stats" in obj:
        return "status"
    if "neighbors" in obj or ("origin_id" in obj and "count" in obj):
        return "neighbors"
    return None


def load_fixture(fam, fixdir):
    fp = fixdir / FIXTURE_FILES[fam]
    if not fp.exists():
        return None
    return structure(json.loads(fp.read_text()))


def compare(fam, obj, fixdir, any_origin=False):
    fx = load_fixture(fam, fixdir)
    if fx is None:
        print(f"[{fam}] no fixture frozen yet - skipped")
        return True
    fixture_obj = json.loads((fixdir / FIXTURE_FILES[fam]).read_text())
    fx_origin = fixture_obj.get("origin_id")
    obj_origin = obj.get("origin_id") if isinstance(obj, dict) else None
    if not any_origin and fx_origin and obj_origin and obj_origin != fx_origin:
        print(f"[{fam}] skipped (origin {obj_origin[:12]}... != fixture {fx_origin[:12]}...)")
        return True
    new = structure(obj)
    added = sorted(set(new) - set(fx))
    removed = sorted(set(fx) - set(new))
    changed = sorted(k for k in set(new) & set(fx) if new[k] != fx[k])
    ok = not (added or removed or changed)
    if ok:
        print(f"[{fam}] OK - {len(new)} keys, no structural change")
    else:
        print(f"[{fam}] CONTRACT CHANGE DETECTED")
        for k in added:
            print(f"  + {k} ({new[k]})")
        for k in removed:
            print(f"  - {k} ({fx[k]})")
        for k in changed:
            print(f"  ~ {k}: {fx[k]} -> {new[k]}")
    return ok


def classify_topic(topic):
    leaf = topic.split("/")[-1]
    if leaf in ("status",):
        return "status"
    if leaf in ("packets", "packet"):
        return "packet"
    if leaf in ("neighbors", "neighbours"):
        return "neighbors"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload", nargs="?", help="payload JSON file")
    ap.add_argument("--capture", help="jsonl file of 'topic payload' lines")
    ap.add_argument("--family", choices=("status", "packet", "neighbors"))
    ap.add_argument("--fixture-dir", default=str(DEFAULT_FIXTURES))
    ap.add_argument("--any-origin", action="store_true",
                    help="do not filter payloads by the fixture origin_id")
    args = ap.parse_args()
    fixdir = Path(args.fixture_dir)
    all_ok = True

    if args.capture:
        n = 0
        for line in Path(args.capture).read_text().splitlines():
            if " " not in line:
                continue
            topic, payload = line.split(" ", 1)
            fam = classify_topic(topic)
            if fam is None:
                continue
            try:
                obj = json.loads(payload)
            except json.JSONDecodeError:
                continue
            n += 1
            all_ok &= compare(fam, obj, fixdir, args.any_origin)
        print(f"checked {n} payloads")
        return 0 if all_ok else 1

    if not args.payload:
        ap.print_help()
        return 2
    obj = json.loads(Path(args.payload).read_text())
    fam = args.family or detect_family(obj)
    if fam is None:
        print("could not detect family; use --family", file=sys.stderr)
        return 2
    return 0 if compare(fam, obj, fixdir, args.any_origin) else 1


if __name__ == "__main__":
    sys.exit(main())
