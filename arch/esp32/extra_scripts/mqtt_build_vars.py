#!/usr/bin/env python3

import os

Import("env")


STRING_OVERRIDES = {
    "MESHCORE_MQTT_ADVERT_NAME": "ADVERT_NAME",
    "MESHCORE_MQTT_ADMIN_PASSWORD": "ADMIN_PASSWORD",
    "MESHCORE_MQTT_WIFI_SSID": "WIFI_SSID",
    "MESHCORE_MQTT_WIFI_PWD": "WIFI_PWD",
    "MESHCORE_MQTT_TOPIC_ROOT": "MQTT_TOPIC_ROOT",
    "MESHCORE_MQTT_URI": "MQTT_URI",
    "MESHCORE_MQTT_USERNAME": "MQTT_USERNAME",
    "MESHCORE_MQTT_PASSWORD": "MQTT_PASSWORD",
    "MESHCORE_MQTT_IATA": "MQTT_IATA",
    "MESHCORE_MQTT_MODEL": "MQTT_MODEL",
    "MESHCORE_MQTT_CLIENT_VERSION": "MQTT_CLIENT_VERSION",
}

RAW_OVERRIDES = {
    "MESHCORE_MQTT_ADVERT_LAT": "ADVERT_LAT",
    "MESHCORE_MQTT_ADVERT_LON": "ADVERT_LON",
    "MESHCORE_MQTT_RETAIN_STATUS": "MQTT_RETAIN_STATUS",
}


def append_define(macro_name, value):
    env.Append(CPPDEFINES=[(macro_name, value)])


def encode_cpp_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '\\"{}\\"'.format(escaped)


applied = []

for env_name, macro_name in STRING_OVERRIDES.items():
    value = os.environ.get(env_name)
    if value is None:
        continue
    append_define(macro_name, encode_cpp_string(value))
    applied.append(env_name)

for env_name, macro_name in RAW_OVERRIDES.items():
    value = os.environ.get(env_name)
    if value is None:
        continue
    append_define(macro_name, value)
    applied.append(env_name)

if applied:
    print("MQTT build overrides:", ", ".join(applied))
