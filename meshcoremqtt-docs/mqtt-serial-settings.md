# MQTT Serial Settings Reference

This page lists every runtime MQTT setting accepted by `set mqtt.<key> <value>` in the firmware used by this repository.

## Broker Model

The firmware supports up to 6 MQTT brokers.

- Broker-specific keys use the form `mqtt.N.<field>`
- `N` must be `1` through `6`
- Unindexed broker keys such as `mqtt.uri` map to broker 1

Recommended practice:

- use `mqtt.1.*` explicitly for broker 1
- use `mqtt.2.*` through `mqtt.6.*` for additional brokers
- avoid the unindexed aliases in new docs and scripts

## Shared Settings

These settings are not broker-specific.

### `mqtt.wifi.ssid`

- Purpose: Wi-Fi SSID used by the MQTT reporter
- Example: `set mqtt.wifi.ssid MyWiFi`
- Stored length: up to 63 characters
- Empty value allowed: yes
- Default: build-time `WIFI_SSID`, usually empty

### `mqtt.wifi.pass`

- Purpose: Wi-Fi password used by the MQTT reporter
- Example: `set mqtt.wifi.pass correct horse battery staple`
- Stored length: up to 63 characters
- Empty value allowed: yes
- Default: build-time `WIFI_PWD`, usually empty

### `mqtt.model`

- Purpose: model string published in MQTT status payloads
- Example: `set mqtt.model Heltec V4`
- Stored length: up to 63 characters
- Empty value allowed: yes, but an empty value is sanitized back to the build default
- Default: build-time `MQTT_MODEL`, fallback `Heltec V3`

### `mqtt.client.version`

- Purpose: client version string published in MQTT status payloads
- Example: `set mqtt.client.version custom-mqtt-observer/1.0.0`
- Stored length: up to 63 characters
- Empty value allowed: yes, but an empty value is sanitized back to the build default
- Default: build-time `MQTT_CLIENT_VERSION`, fallback `custom-mqtt-observer/1.0.0`

## Broker Settings

The following settings are available for each broker index.

Examples on this page use broker 1, but the same fields apply to brokers 2 through 6.

### `mqtt.1.uri`

- Purpose: broker URI
- Example: `set mqtt.1.uri wss://mqtt.ukmesh.com:443/`
- Stored length: up to 127 characters
- Empty value allowed: yes
- Default:
  - broker 1: build-time `MQTT_BROKER1_URI`, which may inherit `MQTT_URI`
  - brokers 2-6: usually empty

Supported URI families in the firmware:

- `ws://`
- `wss://`
- `mqtt://`
- `mqtts://`

The URI string is passed through to the ESP-IDF MQTT client.

### `mqtt.1.username`

- Purpose: MQTT username
- Example: `set mqtt.1.username observer`
- Stored length: up to 63 characters
- Empty value allowed: yes
- Default:
  - broker 1: build-time `MQTT_BROKER1_USERNAME`, which may inherit `MQTT_USERNAME`
  - brokers 2-6: usually empty

### `mqtt.1.password`

- Purpose: MQTT password
- Example: `set mqtt.1.password secret-value`
- Stored length: up to 63 characters
- Empty value allowed: yes
- Default:
  - broker 1: build-time `MQTT_BROKER1_PASSWORD`, which may inherit `MQTT_PASSWORD`
  - brokers 2-6: usually empty

Notes:

- `show mqtt` masks this field as `******`
- `get mqtt.1.password` returns the real stored value

### `mqtt.1.topic.root`

- Purpose: topic root used to derive status and packet topics
- Example: `set mqtt.1.topic.root meshcore/{IATA}/{PUBLIC_KEY}/packets`
- Stored length: up to 255 characters
- Empty value allowed: yes, but an empty value is sanitized back to the build default
- Default:
  - broker 1: build-time `MQTT_BROKER1_TOPIC_ROOT`, which may inherit `MQTT_TOPIC_ROOT`
  - brokers 2-6: usually empty, then sanitized to the build default when saved

Supported tokens:

- `{IATA}` or `<IATA>`
- `{PUBLIC_KEY}` or `<PUBLIC_KEY>`

### `mqtt.1.iata`

- Purpose: site or location code inserted into topics
- Example: `set mqtt.1.iata MME`
- Stored length: up to 15 characters
- Empty value allowed: yes, but an empty value is sanitized back to the build default
- Default:
  - broker 1: build-time `MQTT_BROKER1_IATA`, which may inherit `MQTT_IATA`
  - brokers 2-6: usually empty, then sanitized to the build default when saved

### `mqtt.1.retain.status`

- Purpose: controls the retain flag for status messages and LWT
- Example: `set mqtt.1.retain.status 1`
- Stored values: boolean
- Default: build-time `MQTT_BROKER1_RETAIN_STATUS`, which usually inherits `MQTT_RETAIN_STATUS`

False values accepted by the parser:

- `0`
- `off`
- `false`

Everything else is treated as true.

### `mqtt.1.enabled`

- Purpose: enables or disables connection attempts for that broker
- Example: `set mqtt.1.enabled 1`
- Stored values: boolean
- Default:
  - broker 1: build-time `MQTT_BROKER1_ENABLED`, fallback `1`
  - brokers 2-6: build-time `MQTT_BROKERn_ENABLED`, fallback `0`

False values accepted by the parser:

- `0`
- `off`
- `false`

Everything else is treated as true.

## Legacy Broker 1 Aliases

For compatibility, these keys act on broker 1:

- `mqtt.uri`
- `mqtt.username`
- `mqtt.password`
- `mqtt.topic.root`
- `mqtt.iata`
- `mqtt.retain.status`
- `mqtt.enabled`

Example:

```text
set mqtt.uri wss://mqtt.ukmesh.com:443/
```

That is equivalent to:

```text
set mqtt.1.uri wss://mqtt.ukmesh.com:443/
```

## Persistence

MQTT settings are saved to `/mqtt.cfg` on the device filesystem.

Every successful `set mqtt...` command updates the in-memory config and then saves the full settings block immediately.
