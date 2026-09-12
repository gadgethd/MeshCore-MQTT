#!/usr/bin/env python3
"""Compare ukmesh payloads against the frozen C6 fixtures.

The checker validates the complete JSON shape, including nested objects and
array elements. Values are compared for stable fields; fields whose values
are expected to change during a soak (timestamps, counters, packet contents,
and similar telemetry) are normalized explicitly below. A capture is
accepted only when it actually checks payloads from every frozen family.

Usage:
  scripts/check-ukmesh-payload-fixtures.py NEW.json
  scripts/check-ukmesh-payload-fixtures.py --capture capture.jsonl \
      --allow-missing-neighbors-fixture

Options:
  --family status|packet|neighbors  (default: auto-detect for single files)
  --fixture-dir DIR                 (default: test/fixtures/ukmesh)
  --skip-other-origins              explicitly skip origin_id mismatches
  --allow-missing-neighbors-fixture explicitly acknowledge the unfrozen
                                     neighbors family until a real sample exists

Exit code: 0 = all required payloads were checked and matched, 1 = a contract
or coverage failure, 2 = usage error.
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURES = REPO / "test" / "fixtures" / "ukmesh"
FIXTURE_FILES = {
    "status": "status-online.sample.json",
    "packet": "packet-tx.sample.json",
    "neighbors": "neighbors.sample.json",
}
EXPECTED_FAMILIES = tuple(FIXTURE_FILES)

# These fields are intentionally volatile on a live node. Their types and
# presence remain part of the contract; only their scalar values are ignored.
# Keep this list explicit so adding a new dynamic field is a visible contract
# decision rather than an accidental weakening of the comparator.
VOLATILE_FIELD_NAMES = frozenset(
    {
        "origin",
        "origin_id",
        "firmware_version",
        "client_version",
        "timestamp",
        "time",
        "date",
        "raw",
        "hash",
        "len",
        "packet_type",
        "payload_len",
        "uptime_ms",
        "boot_count",
        "reset_reason",
        "ntp_synced",
        "ntp_sync_age_ms",
        "boot_epoch",
        "max_loop_ms",
        "max_loop_at_ms",
        "loop_iterations",
        "wifi_reconnect_attempts",
        "rx_publish_calls",
        "tx_publish_calls",
        "tx_fail_publish_calls",
        "publish_skipped_no_connection",
        "forward_successes",
        "forward_successes_flood",
        "forward_successes_direct",
        "forward_failures",
        "tx_queue_depth",
        "tx_queue_depth_peak",
        "heap_free",
        "heap_min_free",
        "heap_min_seen_since_boot",
        "wifi_connected",
        "wifi_uptime_ms",
        "wifi_rssi",
        "wifi_ssid",
        "nodes_heard_24h",
        "last_rx_rssi",
        "last_rx_snr",
        "last_tx_fail_reason",
        "config_crc32",
        "fs_free_bytes",
        "fs_total_bytes",
        "nvs_free_entries",
        "power_source",
        "solar_mv",
        "board_temp_c",
        "git_commit",
        "idle_pct_core0",
        "idle_pct_core1",
        "battery_mv",
        "uptime_secs",
        "tx_air_secs",
        "rx_air_secs",
        "channel_utilization",
        "air_util_tx",
        "air_util_rx",
        "broker_index",
        "connect_attempts",
        "connect_start_failures",
        "connect_events",
        "disconnect_events",
        "error_events",
        "reconnect_rung",
        "reconnect_breaker",
        "reconnect_next_in_ms",
        "last_error_type",
        "last_error_code",
        "heap_inactive",
        "broker_uri",
        "broker_username",
        "reconnect_attempts_1h",
        "status_publishes",
        "packet_publishes",
        "session_status_publishes",
        "session_packet_publishes",
        "publish_failures",
        "publish_queue_depth",
        "publish_queue_drops",
        "publish_outbox_drops",
        "connected",
        "neighbor_interval_s",
        "last_offline_epoch",
        # Future neighbors samples will contain these live measurements.
        "node_id",
        "rssi",
        "snr",
        "last_heard",
    }
)

# Dynamic fields can still have a constrained contract value. This catches a
# typo such as status="stale" while allowing online/offline transitions.
VALUE_DOMAINS = {
    "status": frozenset({"online", "offline"}),
    "direction": frozenset({"tx", "rx"}),
}

_VOLATILE = object()
_NON_LEAF = object()


def type_class(value):
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, dict):
        return "object"
    if isinstance(value, list):
        return "array"
    if value is None:
        return "null"
    return type(value).__name__


def _join_path(prefix, component):
    if not prefix:
        return component
    if component.startswith("["):
        return f"{prefix}{component}"
    return f"{prefix}.{component}"


def _field_name(path):
    leaf = path.rsplit(".", 1)[-1]
    return leaf.split("[", 1)[0]


def _stable_value(value, path):
    if isinstance(value, (dict, list)):
        return _NON_LEAF
    if _field_name(path) in VOLATILE_FIELD_NAMES:
        return _VOLATILE
    return value


def contract_entries(value, prefix=""):
    """Flatten JSON shape and stable scalar values into comparable entries."""

    entries = {}
    if isinstance(value, dict):
        for key, child in value.items():
            path = _join_path(prefix, str(key))
            entries[path] = (type_class(child), _stable_value(child, path))
            entries.update(contract_entries(child, path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            path = _join_path(prefix, f"[{index}]")
            entries[path] = (type_class(child), _stable_value(child, path))
            entries.update(contract_entries(child, path))
    elif not prefix:
        # A scalar root is invalid for the payload families, but retain its
        # type so that it cannot compare equal to an empty object by accident.
        entries["$"] = (type_class(value), _stable_value(value, "$"))
    return entries


def _display(value):
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    except TypeError:
        return repr(value)


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


def classify_topic(topic):
    leaf = topic.rstrip("/").split("/")[-1]
    if leaf == "status":
        return "status"
    if leaf in ("packets", "packet"):
        return "packet"
    if leaf in ("neighbors", "neighbours"):
        return "neighbors"
    return None


def load_fixtures(fixdir):
    fixtures = {}
    errors = {}
    for family, filename in FIXTURE_FILES.items():
        path = fixdir / filename
        if not path.exists():
            errors[family] = f"fixture is missing: {path}"
            continue
        try:
            fixtures[family] = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors[family] = f"fixture cannot be read as JSON ({path}): {exc}"
    return fixtures, errors


def compare(family, obj, fixture_obj, skip_other_origins=False):
    if fixture_obj is None:
        print(f"[{family}] ERROR: no fixture is available; nothing was checked")
        return "missing_fixture"

    if not isinstance(obj, dict):
        print(f"[{family}] CONTRACT CHANGE DETECTED: payload root is {type_class(obj)}, expected object")
        return "mismatch"
    if not isinstance(fixture_obj, dict):
        print(f"[{family}] ERROR: fixture root is {type_class(fixture_obj)}, expected object")
        return "mismatch"

    fixture_origin = fixture_obj.get("origin_id")
    payload_origin = obj.get("origin_id")
    origin_mismatch = (
        fixture_origin is not None
        and payload_origin is not None
        and fixture_origin != payload_origin
    )
    if origin_mismatch and skip_other_origins:
        print(
            f"[{family}] SKIPPED origin {str(payload_origin)[:12]}... != "
            f"fixture {str(fixture_origin)[:12]}..."
        )
        return "skipped_origin"

    fixture_entries = contract_entries(fixture_obj)
    payload_entries = contract_entries(obj)
    added = sorted(set(payload_entries) - set(fixture_entries))
    removed = sorted(set(fixture_entries) - set(payload_entries))
    changed_types = sorted(
        path
        for path in set(payload_entries) & set(fixture_entries)
        if payload_entries[path][0] != fixture_entries[path][0]
    )
    changed_values = sorted(
        path
        for path in set(payload_entries) & set(fixture_entries)
        if payload_entries[path][0] == fixture_entries[path][0]
        and _field_name(path) not in VALUE_DOMAINS
        and payload_entries[path][1] not in (_VOLATILE, _NON_LEAF)
        and fixture_entries[path][1] not in (_VOLATILE, _NON_LEAF)
        and payload_entries[path][1] != fixture_entries[path][1]
    )
    invalid_values = []
    for path, (_, value) in payload_entries.items():
        domain = VALUE_DOMAINS.get(_field_name(path))
        if domain is not None and value not in (_VOLATILE, _NON_LEAF) and value not in domain:
            invalid_values.append((path, value, domain))

    ok = not (
        origin_mismatch
        or added
        or removed
        or changed_types
        or changed_values
        or invalid_values
    )
    if ok:
        print(f"[{family}] OK - {len(payload_entries)} contract entries matched")
        return "ok"

    print(f"[{family}] CONTRACT CHANGE DETECTED")
    if origin_mismatch:
        print(
            f"  ~ origin_id: {fixture_origin} -> {payload_origin} "
            "(use --skip-other-origins to explicitly skip)"
        )
    for path in added:
        print(f"  + {path} ({payload_entries[path][0]})")
    for path in removed:
        print(f"  - {path} ({fixture_entries[path][0]})")
    for path in changed_types:
        print(f"  ~ {path}: {fixture_entries[path][0]} -> {payload_entries[path][0]}")
    for path in changed_values:
        print(
            f"  ~ {path}: {_display(fixture_entries[path][1])} -> "
            f"{_display(payload_entries[path][1])}"
        )
    for path, value, domain in sorted(invalid_values):
        print(f"  ! {path}: {_display(value)} (expected one of {sorted(domain)})")
    return "mismatch"


def print_summary(stats, family_counts=None, extra=None):
    parts = [
        f"records={stats.get('records', 0)}",
        f"checked={stats.get('checked', 0)}",
        f"passed={stats.get('passed', 0)}",
        f"failed={stats.get('failed', 0)}",
        f"skipped_origin={stats.get('skipped_origin', 0)}",
        f"unvalidated={stats.get('unvalidated', 0)}",
        f"malformed={stats.get('malformed', 0)}",
        f"unknown_topics={stats.get('unknown_topics', 0)}",
        f"missing_fixtures={stats.get('missing_fixtures', 0)}",
        f"missing_families={stats.get('missing_families', 0)}",
    ]
    if family_counts:
        families = ",".join(
            f"{family}:{family_counts.get(family, 0)}" for family in EXPECTED_FAMILIES
        )
        parts.append(f"families={families}")
    if extra:
        parts.extend(extra)
    print("Summary: " + ", ".join(parts))


def _read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except (OSError, json.JSONDecodeError) as exc:
        return None, str(exc)


def run_single(args, fixdir):
    stats = Counter()
    payload_path = Path(args.payload)
    obj, error = _read_json(payload_path)
    if error is not None:
        print(f"ERROR: payload cannot be read as JSON ({payload_path}): {error}", file=sys.stderr)
        print_summary(stats)
        return 1

    family = args.family or detect_family(obj)
    if family is None:
        print("ERROR: could not detect family; use --family", file=sys.stderr)
        print_summary(stats)
        return 2

    fixtures, fixture_errors = load_fixtures(fixdir)
    if "neighbors" in fixture_errors and family != "neighbors":
        print(
            "WARNING: neighbors fixture is not frozen yet; neighbors coverage is "
            "unvalidated. Add a real sample or use the explicit capture placeholder policy.",
            file=sys.stderr,
        )
    if family in fixture_errors:
        print(f"[{family}] ERROR: {fixture_errors[family]}", file=sys.stderr)
        stats["missing_fixtures"] += 1
        print_summary(stats)
        return 1

    stats["records"] = 1
    outcome = compare(
        family,
        obj,
        fixtures[family],
        skip_other_origins=args.skip_other_origins,
    )
    if outcome == "ok":
        stats["checked"] = 1
        stats["passed"] = 1
    elif outcome == "skipped_origin":
        stats["skipped_origin"] = 1
    elif outcome == "missing_fixture":
        stats["missing_fixtures"] = 1
    else:
        stats["checked"] = 1
        stats["failed"] = 1
    print_summary(stats, {family: 1})
    # A single-payload invocation that was skipped has checked nothing.
    return 0 if outcome == "ok" else 1


def run_capture(args, fixdir):
    stats = Counter()
    family_counts = Counter()
    fixtures, fixture_errors = load_fixtures(fixdir)
    preflight_failed = False

    for family in EXPECTED_FAMILIES:
        error = fixture_errors.get(family)
        if error is None:
            continue
        stats["missing_fixtures"] += 1
        if family == "neighbors" and args.allow_missing_neighbors_fixture:
            print(
                f"WARNING: [{family}] {error}; explicitly allowed as an "
                "unvalidated placeholder (no payload contract is asserted).",
                file=sys.stderr,
            )
        else:
            print(
                f"ERROR: [{family}] {error}. Missing families/fixtures are a "
                "failure; use --allow-missing-neighbors-fixture only for the "
                "documented unfrozen-neighbors placeholder.",
                file=sys.stderr,
            )
            preflight_failed = True

    try:
        lines = Path(args.capture).read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        print(f"ERROR: capture cannot be read ({args.capture}): {exc}", file=sys.stderr)
        print_summary(stats, family_counts)
        return 1

    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped:
            continue
        stats["records"] += 1
        fields = stripped.split(None, 1)
        if len(fields) != 2:
            stats["malformed"] += 1
            print(f"ERROR: capture line {line_number} has no topic/payload separator", file=sys.stderr)
            continue
        topic, payload = fields
        try:
            obj = json.loads(payload)
        except json.JSONDecodeError as exc:
            stats["malformed"] += 1
            print(f"ERROR: capture line {line_number} has malformed JSON: {exc}", file=sys.stderr)
            continue

        family = classify_topic(topic)
        if family is None:
            stats["unknown_topics"] += 1
            print(f"ERROR: capture line {line_number} has an unknown topic: {topic}", file=sys.stderr)
            continue

        family_counts[family] += 1
        if family in fixture_errors:
            if family == "neighbors" and args.allow_missing_neighbors_fixture:
                stats["unvalidated"] += 1
                print(
                    f"[{family}] UNVALIDATED line {line_number}: no fixture sample exists",
                    file=sys.stderr,
                )
            else:
                stats["failed"] += 1
                print(f"[{family}] ERROR: {fixture_errors[family]}", file=sys.stderr)
            continue

        outcome = compare(
            family,
            obj,
            fixtures[family],
            skip_other_origins=args.skip_other_origins,
        )
        if outcome == "ok":
            stats["checked"] += 1
            stats["passed"] += 1
        elif outcome == "skipped_origin":
            stats["skipped_origin"] += 1
        elif outcome == "missing_fixture":
            stats["missing_fixtures"] += 1
            stats["failed"] += 1
        else:
            stats["checked"] += 1
            stats["failed"] += 1

    for family in EXPECTED_FAMILIES:
        if family_counts[family] != 0:
            continue
        if family == "neighbors" and "neighbors" in fixture_errors and args.allow_missing_neighbors_fixture:
            print(
                "WARNING: [neighbors] no payload was present and no real fixture "
                "exists; this family remains explicitly unvalidated.",
                file=sys.stderr,
            )
            continue
        stats["missing_families"] += 1
        preflight_failed = True
        print(f"ERROR: capture has no {family} payload family", file=sys.stderr)

    if stats["checked"] == 0:
        preflight_failed = True
        print("ERROR: capture checked zero payloads; acceptance is fail-closed", file=sys.stderr)

    print_summary(stats, family_counts)
    return 0 if not preflight_failed and stats["failed"] == 0 and stats["malformed"] == 0 and stats["unknown_topics"] == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload", nargs="?", help="payload JSON file")
    ap.add_argument("--capture", help="jsonl file of 'topic payload' lines")
    ap.add_argument("--family", choices=EXPECTED_FAMILIES)
    ap.add_argument("--fixture-dir", default=str(DEFAULT_FIXTURES))
    origins = ap.add_mutually_exclusive_group()
    origins.add_argument(
        "--skip-other-origins",
        action="store_true",
        help="explicitly skip payloads whose origin_id differs from the fixture",
    )
    origins.add_argument(
        "--any-origin",
        action="store_true",
        help="compare every origin (legacy spelling; this is the strict default)",
    )
    ap.add_argument(
        "--allow-missing-neighbors-fixture",
        action="store_true",
        help="acknowledge that neighbors has no real sample yet; those payloads remain unvalidated",
    )
    args = ap.parse_args()

    if args.capture and args.payload:
        ap.error("provide either payload or --capture, not both")
    if args.capture and args.family:
        ap.error("--family is only valid for a single payload")

    fixdir = Path(args.fixture_dir)
    if args.capture:
        return run_capture(args, fixdir)
    if not args.payload:
        ap.print_help()
        return 2
    return run_single(args, fixdir)


if __name__ == "__main__":
    sys.exit(main())
