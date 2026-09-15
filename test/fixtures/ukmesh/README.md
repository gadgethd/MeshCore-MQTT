# ukmesh payload fixtures (C6)

Frozen samples of the wire payloads our firmware publishes to the ukmesh
broker, refreshed from a fresh read-only test-lane capture on 2026-09-15.
The observed node is the Heltec V3 "MQTT test" node running v1.17.1. These
samples lock the **payload contract**:
topic families `{root}/{iata}/{public_key}/{status,packets,neighbors}` and the
JSON structure of each family, so no refactor can silently change what ukmesh
consumers receive.

## Files

- `status-online.sample.json` - one status payload (retained family).
  **Sanitized:** `stats.wifi_ssid` is replaced with `REDACTED`; no other
  fields carry site-identifying data. Do not un-redact.
- `packet-tx.sample.json` - one outbound packet record.
- `packet-rx.sample.json` - one inbound packet record, including the
  receive-only `SNR`, `RSSI`, `score`, and `duration` measurements.
- `capture.sample.jsonl` - the sanitized status/tx/rx lines used by CI for
  comparator acceptance. Each line is copied from the 2026-09-15 capture;
  only `stats.wifi_ssid` is redacted.
- `neighbors` fixture: to be added when a live sample is captured (the node
  publishes neighbours on its own schedule).

The v1.17.1 refresh expands `stats` to 65 keys. It records the new clock/NTP,
internal/PSRAM heap, build/serialization, transport/TLS, error, queue-byte,
and outbox diagnostics captured from the device. Packet contracts are now
direction-specific: tx remains the 13-key shape and rx is a separate 17-key
shape. The captured `client_version` remains `meshcore-mqtt/v1.17.0`; that
live value is retained rather than synthesized.

Only genuinely soak-dynamic additions are normalized by the comparator:
runtime clock outcome/source flags and attempt state; heap measurements;
build/serialization and error/drop counters; last-error codes/times/ages;
live TLS count; current queue/outbox byte counts; and the four rx radio
measurements. Stable policy and capacity additions remain value-frozen:
`ntp_validation_mode`, `effective_transport`, `tls_cap`, `publish_queue_cap`,
`publish_queue_byte_cap`, `publish_queue_total_byte_cap`, and
`publish_outbox_cap`.

The capture still contained no neighbors payload. No synthetic neighbors or
other live values are added here.

## Enforcement

- `test/test_ukmesh_payload_fixtures/` (native gtest) asserts the fixtures
  stay well-formed and carry the expected key sets; it runs in CI via
  `pio test -e native`.
- `scripts/check-ukmesh-payload-fixtures.py` compares *new* captures against
  these fixtures. It checks added/removed keys, type-class changes, stable
  scalar values, and every nested array element. Tx and rx packet shapes are
  selected by `direction` and both must be present in a capture. Only the
  explicitly listed volatile telemetry fields are normalized. Malformed
  capture lines, missing contracts, and zero checked payloads fail closed; the
  command prints a count summary. Use it during soaks / after port waves:

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
