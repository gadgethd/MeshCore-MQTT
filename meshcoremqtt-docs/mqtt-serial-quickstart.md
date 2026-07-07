# MQTT Serial Quick Start

This page shows the fastest reliable way to configure the MQTT-enabled repeater firmware in this repository over the serial console.

## Applies To

- ESP32 builds with `WITH_MQTT_REPORTER`
- the serial console exposed by USB CDC or UART

All MQTT configuration commands in this firmware are serial-only. They are not accepted as remote mesh commands.

## Serial Connection

The firmware and helper tooling in this repository use `115200` baud.

Typical options:

- `screen /dev/ttyACM0 115200`
- `picocom -b 115200 /dev/ttyACM0`
- `python3 MeshCore/bin/configure-device.py /dev/ttyACM0`

Commands are terminated with carriage return. Most serial terminals send a usable line ending by default. The helper script sends `\r`.

## Minimum Working Setup For Broker 1

Send these commands one by one:

```text
set mqtt.wifi.ssid YOUR_WIFI_SSID
set mqtt.wifi.pass YOUR_WIFI_PASSWORD

set mqtt.1.uri wss://mqtt.ukmesh.com:443/
set mqtt.1.username YOUR_MQTT_USERNAME
set mqtt.1.password YOUR_MQTT_PASSWORD
set mqtt.1.topic.root meshcore/{IATA}/{PUBLIC_KEY}/packets
set mqtt.1.iata MME
set mqtt.1.retain.status 1
set mqtt.1.enabled 1

set mqtt.model Heltec V4
set mqtt.client.version custom-mqtt-observer/1.0.0

mqtt reconnect 1
show mqtt.1
show mqtt stats.1
```

## What To Expect

- Each successful `set mqtt...` command replies with `OK`.
- `show mqtt.1` prints the saved configuration for broker 1.
- `show mqtt stats.1` prints connection counters and whether that broker is currently connected.

## Important Notes

- `mqtt.1.*` means broker 1. The firmware supports brokers `1` through `6`.
- Unindexed broker keys such as `mqtt.uri` and `mqtt.username` are treated as broker 1 for compatibility, but the indexed form is clearer and is the recommended form for documentation and scripts.
- Settings are saved immediately after each `set mqtt...` command.
- Changing settings does not always force a live reconnect. Run `mqtt reconnect 1` after updating broker settings.
- If you use `wss://` or `mqtts://`, the device needs valid time before TLS can connect. The firmware attempts NTP sync automatically.

## Verify The Whole MQTT State

Use these commands when checking a device after setup:

```text
show mqtt
show mqtt stats
get mqtt.1.uri
get mqtt.1.enabled
```

`show mqtt` masks stored passwords. `get mqtt.1.password` returns the real saved password, so use that carefully.
