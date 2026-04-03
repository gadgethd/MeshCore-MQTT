# MQTT Serial Command Reference

This page documents the MQTT command surface implemented by the firmware in this repository.

## Command Scope

All commands on this page are serial-only.

If the same text is sent as a remote mesh command, the firmware rejects it with `Err - serial only`.

## Show Commands

### `show mqtt`

Print the shared MQTT settings and each broker that is enabled or has a URI configured.

Example:

```text
show mqtt
```

### `get mqtt`

Alias for `show mqtt`.

Example:

```text
get mqtt
```

### `show mqtt.1`

Print the configuration for one broker.

Broker numbers are `1` to `6`.

Examples:

```text
show mqtt.1
show mqtt.2
```

### `show mqtt stats`

Print MQTT runtime counters, heap stats, Wi-Fi state, and per-broker stats for configured brokers.

Example:

```text
show mqtt stats
```

### `show mqtt stats.1`

Print stats for a single broker.

Examples:

```text
show mqtt stats.1
show mqtt stats.2
```

## Get Commands

### `get mqtt.<key>`

Read a single saved MQTT value.

Examples:

```text
get mqtt.wifi.ssid
get mqtt.model
get mqtt.1.uri
get mqtt.1.enabled
```

Notes:

- Shared keys use names like `mqtt.wifi.ssid`.
- Broker keys use names like `mqtt.1.uri`.
- Unindexed broker keys such as `mqtt.uri` map to broker 1.
- `get mqtt.1.password` returns the real stored password.

## Set Commands

### `set mqtt.<key> <value>`

Store a single MQTT setting and save it to device storage immediately.

Examples:

```text
set mqtt.wifi.ssid MyWiFi
set mqtt.model Heltec V4
set mqtt.1.uri wss://mqtt.ukmesh.com:443/
set mqtt.1.enabled 1
```

Notes:

- The parser splits on the first space after the key, so values may contain spaces.
- Leading spaces before the value are ignored.
- Any failed save returns `Err - save failed`.
- Unknown keys return `Err - save failed` because key validation and persistence are handled together.

## Reconnect Commands

### `mqtt reconnect`

Reset all MQTT connections and let the firmware reconnect using the saved config.

Example:

```text
mqtt reconnect
```

### `mqtt reconnect 1`

Reset one broker connection.

Examples:

```text
mqtt reconnect 1
mqtt reconnect 2
```

Use this after changing broker settings if you want the new values applied immediately.

## Reset Command

### `mqtt reset`

Reset MQTT settings back to the firmware defaults compiled into the build, save them, and reset MQTT connections.

Example:

```text
mqtt reset
```

Important:

- This does not necessarily clear everything to empty strings.
- The result depends on the build-time defaults included in the firmware image.

## Common Responses

- `OK`: command accepted and completed
- `Err - serial only`: command was not issued on the serial console
- `Err - broker 1-6`: invalid broker number
- `Err - bad params`: malformed `set mqtt...` command
- `Err - unknown mqtt key`: invalid `get mqtt.<key>` key
- `Err - save failed`: invalid `set mqtt.<key>` key or storage save failure
