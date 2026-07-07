# MeshcoreMQTT Docs

This folder contains MeshcoreMQTT-specific documentation.

It is separate from the upstream `MeshCore/docs` tree on purpose. The pages here document the MQTT-enabled repeater workflow used in this repository, especially serial configuration of the MQTT reporter.

## Contents

- [MQTT Serial Quick Start](./mqtt-serial-quickstart.md)
- [MQTT Serial Command Reference](./mqtt-serial-commands.md)
- [MQTT Serial Settings Reference](./mqtt-serial-settings.md)
- [MQTT Serial Behavior and Examples](./mqtt-serial-behavior.md)

## Scope

These docs cover:

- how to connect to the device over serial
- which MQTT commands the firmware accepts
- every `mqtt.*` setting that can be configured at runtime
- broker indexing rules such as `mqtt.1.*`
- runtime behavior that affects setup, including reconnects, topic token expansion, and secure broker time sync requirements

These docs do not try to replace the upstream MeshCore documentation for general CLI usage, radio settings, or unrelated firmware features.
