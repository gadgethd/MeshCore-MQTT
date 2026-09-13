# ukmesh payload fixtures (C6)

Frozen samples of the wire payloads our firmware publishes to the ukmesh
broker, refreshed from a fresh read-only test-lane capture on 2026-09-13.
The observed node is the Heltec V3 "MQTT test" node running v1.17.0. These
samples lock the **payload contract**:
topic families `{root}/{iata}/{public_key}/{status,packets,neighbors}` and the
JSON structure of each family, so no refactor can silently change what ukmesh
consumers receive.

## Files

- `status-online.sample.json` - one status payload (retained family).
  **Sanitized:** `stats.wifi_ssid` is replaced with `REDACTED`; no other
  fields carry site-identifying data. Do not un-redact.
- `packet-tx.sample.json` - one outbound packet record.
- `capture.sample.jsonl` - the sanitized status/packet lines used by CI for
  comparator acceptance.
- `neighbors` fixture: to be added when a live sample is captured (the node
  publishes neighbours on its own schedule).

The capture contained no neighbors payload, and the observed v1.17.0 image
does not emit the newer clock/TLS/heap/queue-byte/outbox diagnostic fields
implemented by the current builder. Those current fields are covered by the
native production-builder tests; the live fixture will be refreshed again when
the test-lane image publishes them. No synthetic live values are added here.

## Enforcement

- `test/test_ukmesh_payload_fixtures/` (native gtest) asserts the fixtures
  stay well-formed and carry the expected key sets; it runs in CI via
  `pio test -e native`.
- `scripts/check-ukmesh-payload-fixtures.py` compares *new* captures against
  these fixtures. It checks added/removed keys, type-class changes, stable
  scalar values, and every nested array element. Only the explicitly listed
  volatile telemetry fields are normalized. Malformed capture lines, missing
  families, and zero checked payloads fail closed; the command prints a count
  summary. Use it during soaks / after port waves:

      scripts/check-ukmesh-payload-fixtures.py --capture capture.jsonl \
          --allow-missing-neighbors-fixture

  The `--skip-other-origins` option is required when a multi-node capture is
  intentionally filtered by the fixture origin; skipped records are counted.

## Re-capturing / updating

Capture from the broker side (backend read creds) while the node publishes:

    mosquitto_sub -h <broker> -t 'meshcore-test/TST/<PUBLIC_KEY>/#' -v -q 1

Update a fixture only deliberately: re-capture, sanitize (see above), replace
the file, and update the expected key lists in the native test in the same
change. The contract outranks any port convenience.

The `neighbors` sample is intentionally still unfrozen because no live sample
has been captured. Until it exists, the checker requires the explicit
`--allow-missing-neighbors-fixture` acknowledgement in capture mode, loudly
reports the family as unvalidated, and never treats a neighbors payload as
checked. Remove that option as soon as `neighbors.sample.json` is added.
