import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


def environment_section(text, name):
    match = re.search(
        rf"^\[env:{re.escape(name)}\]\n(?P<body>.*?)(?=^\[|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing PlatformIO environment {name}")
    return match.group("body")


class MqttOtaSecurityTests(unittest.TestCase):
    def test_mqtt_ota_requires_explicit_opt_in(self):
        source = read("src/helpers/ESP32Board.cpp")
        self.assertIn("defined(ENABLE_WIFI_OTA)", source)
        self.assertIn("!defined(WITH_MQTT_REPORTER)", source)
        self.assertNotIn("WiFi.localIP().toString()", source)

    def test_all_http_routes_require_the_ephemeral_credential(self):
        source = read("src/helpers/ESP32Board.cpp")
        for route in ('ota_server->on("/",', 'ota_server->on("/log",'):
            route_start = source.index(route)
            route_end = source.index("});", route_start)
            self.assertIn("requireOTAAuthentication(request)", source[route_start:route_end])
        self.assertIn("ota_server->onNotFound", source)
        self.assertIn(
            "AsyncElegantOTA.begin(ota_server, OTA_USERNAME, ota_password)",
            source,
        )

        elegant_ota = read("arch/esp32/AsyncElegantOTA/src/AsyncElegantOTA.cpp")
        self.assertIn("if(strlen(username) > 0)", elegant_ota)
        self.assertGreaterEqual(elegant_ota.count("request->authenticate"), 4)

    def test_ota_is_isolated_credentialed_and_time_limited(self):
        source = read("src/helpers/ESP32Board.cpp")
        self.assertIn("constexpr size_t OTA_SECRET_BYTES = 16", source)
        self.assertIn("esp_fill_random", source)
        self.assertIn("WiFi.mode(WIFI_AP)", source)
        self.assertIn("WiFi.disconnect(false, false)", source)
        self.assertIn("ota_server->end()", source)
        self.assertIn("WiFi.softAPdisconnect(true)", source)
        self.assertIn("OTA_WINDOW_MS = 10UL * 60UL * 1000UL", source)

        repeater = read("examples/simple_repeater/main.cpp")
        self.assertIn("board.serviceOTAUpdate();", repeater)
        self.assertIn("if (!board.isOTAUpdateActive())", repeater)
        self.assertIn("board.serviceOTAUpdate();", read("examples/simple_room_server/main.cpp"))
        self.assertIn("board.serviceOTAUpdate();", read("examples/simple_sensor/main.cpp"))

    def test_production_mqtt_builds_disable_ota(self):
        self.assertIn(
            "MESHCORE_MQTT_ENABLE_OTA=0",
            read("bin/ci-build-mqtt-repeater.sh"),
        )

        mqtt_environments = (
            ("variants/heltec_v3/platformio.ini", "Heltec_v3_repeater_mqtt"),
            ("variants/heltec_v4/platformio.ini", "heltec_v4_repeater_mqtt"),
            ("variants/lilygo_tbeam_1w/platformio.ini", "LilyGo_TBeam_1W_repeater_mqtt"),
        )
        for path, name in mqtt_environments:
            section = environment_section(read(path), name)
            self.assertIn("-D WITH_MQTT_REPORTER=1", section)
            self.assertIn("-D DISABLE_WIFI_OTA=1", section)
            self.assertIn("${esp32_ota.lib_deps}", section)

    def test_operator_can_explicitly_enable_ota(self):
        overrides = read("arch/esp32/extra_scripts/mqtt_build_vars.py")
        self.assertIn('"MESHCORE_MQTT_ENABLE_OTA": "ENABLE_WIFI_OTA"', overrides)

        builder = read("bin/build-meshcore-mqtt-observer.sh")
        self.assertIn('MESHCORE_MQTT_ENABLE_OTA:-0', builder)
        self.assertIn('ota_flag="  -D ENABLE_WIFI_OTA=1"', builder)


if __name__ == "__main__":
    unittest.main()
