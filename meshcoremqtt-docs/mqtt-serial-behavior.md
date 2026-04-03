# MQTT Serial Behavior and Examples

This page documents the runtime behavior around MQTT settings so operators know what to expect after changing values over serial.

## Connection Rules

The firmware only starts a broker connection when both of these are true:

- `mqtt.N.enabled` is true
- `mqtt.N.uri` is not empty

If either condition is false, that broker is skipped.

## Secure Broker Time Sync

When a broker URI starts with `wss://` or `mqtts://`, the firmware waits for NTP time sync before it attempts the TLS connection.

What this means in practice:

- plain `ws://` or `mqtt://` brokers can connect without time sync
- secure brokers need working DNS and NTP
- immediately after boot, a secure broker may appear idle until time sync completes

The firmware uses `pool.ntp.org` for NTP by default.

## Topic Expansion

The firmware expands tokens in `mqtt.N.topic.root` before it builds topic paths.

Supported token forms:

- `{IATA}`
- `<IATA>`
- `{PUBLIC_KEY}`
- `<PUBLIC_KEY>`

Example input:

```text
meshcore/{IATA}/{PUBLIC_KEY}/packets
```

Example result:

```text
meshcore/MME/ABCDEF12/packets
```

## Status And Packet Topic Derivation

The firmware derives the per-broker status and packet topics from `mqtt.N.topic.root`.

If the topic root ends with `/packets`:

- packet publishes use that path as-is
- status publishes swap the suffix to `/status`

If the topic root ends with `/status`:

- status publishes use that path as-is
- packet publishes swap the suffix to `/packets`

If the topic root ends with neither suffix:

- both topic builders use the topic root unchanged

Recommended practice:

- set `mqtt.N.topic.root` to a path ending in `/packets`
- let the firmware derive the matching `/status` path automatically

## Password Visibility

There is an important difference between `show` and `get`:

- `show mqtt` and `show mqtt.N` mask passwords as `******`
- `get mqtt.N.password` returns the real stored value

Use `show` for routine checks and reserve `get` for explicit credential verification.

## Reset Behavior

`mqtt reset` restores the MQTT settings to the build-time defaults included in the flashed firmware image.

That means:

- values may reset to non-empty defaults
- broker 1 may come back enabled by default
- model, client version, topic root, and IATA may snap back to compiled defaults rather than staying empty

## Empty String Behavior

Some settings stay empty if you set them to an empty string. Others are sanitized back to defaults.

Can remain empty:

- `mqtt.wifi.ssid`
- `mqtt.wifi.pass`
- `mqtt.N.uri`
- `mqtt.N.username`
- `mqtt.N.password`

Revert to build defaults when set empty:

- `mqtt.model`
- `mqtt.client.version`
- `mqtt.N.topic.root`
- `mqtt.N.iata`

## Example: Configure Broker 1

```text
set mqtt.wifi.ssid SiteWiFi
set mqtt.wifi.pass SitePass
set mqtt.1.uri wss://mqtt.ukmesh.com:443/
set mqtt.1.username node1
set mqtt.1.password secret1
set mqtt.1.topic.root meshcore/{IATA}/{PUBLIC_KEY}/packets
set mqtt.1.iata MME
set mqtt.1.retain.status 1
set mqtt.1.enabled 1
mqtt reconnect 1
show mqtt.1
show mqtt stats.1
```

## Example: Add Broker 2

```text
set mqtt.2.uri mqtt://192.168.1.50:1883
set mqtt.2.username repeater2
set mqtt.2.password secret2
set mqtt.2.topic.root meshcore/lab/{PUBLIC_KEY}/packets
set mqtt.2.iata LAB
set mqtt.2.retain.status 0
set mqtt.2.enabled 1
mqtt reconnect 2
show mqtt.2
```

## Example: Disable A Broker Without Deleting It

```text
set mqtt.2.enabled 0
mqtt reconnect 2
```

This keeps the saved URI and credentials but stops connection attempts for broker 2.

## Example: Inspect All MQTT State

```text
show mqtt
show mqtt stats
```

Use this when verifying a flashed device before deployment.
