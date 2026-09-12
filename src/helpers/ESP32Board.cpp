#ifdef ESP_PLATFORM

#include "ESP32Board.h"
#include <target.h>

// Existing non-MQTT repeaters retain their explicit admin-enabled OTA capability.
// MQTT firmware must opt in separately; ENABLE_WIFI_OTA intentionally takes
// precedence over the production DISABLE_WIFI_OTA default.
#if defined(ENABLE_WIFI_OTA) || (defined(ADMIN_PASSWORD) && !defined(WITH_MQTT_REPORTER) && !defined(DISABLE_WIFI_OTA))
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#include <SPIFFS.h>
#include <helpers/MQTTOTABarrier.h>

#if defined(WITH_MQTT_REPORTER)
extern "C" bool meshcore_mqtt_stop_for_ota();
extern "C" bool meshcore_mqtt_can_flash_after_stop();
extern "C" bool meshcore_mqtt_resume_after_ota_abort();
#endif

namespace {

#ifndef WIFI_OTA_WINDOW_MS
constexpr uint32_t OTA_WINDOW_MS = 10UL * 60UL * 1000UL;
#else
constexpr uint32_t OTA_WINDOW_MS = WIFI_OTA_WINDOW_MS;
#endif

static_assert(OTA_WINDOW_MS >= 60UL * 1000UL, "WIFI_OTA_WINDOW_MS must be at least one minute");
static_assert(OTA_WINDOW_MS <= 60UL * 60UL * 1000UL, "WIFI_OTA_WINDOW_MS must not exceed one hour");

constexpr char OTA_AP_SSID[] = "MeshCore-OTA";
constexpr char OTA_USERNAME[] = "meshcore-ota";
constexpr size_t OTA_SECRET_BYTES = 16;
constexpr size_t OTA_SECRET_CHARS = OTA_SECRET_BYTES * 2;

AsyncWebServer* ota_server = nullptr;
uint32_t ota_started_at = 0;
wifi_mode_t ota_previous_wifi_mode = WIFI_OFF;
bool ota_previous_inhibit_sleep = false;
bool ota_active = false;
char ota_password[OTA_SECRET_CHARS + 1];
MQTTOTA::Barrier ota_barrier;

void generateOTASecret(char destination[OTA_SECRET_CHARS + 1]) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  uint8_t random_bytes[OTA_SECRET_BYTES];
  esp_fill_random(random_bytes, sizeof(random_bytes));

  for (size_t i = 0; i < sizeof(random_bytes); i++) {
    destination[i * 2] = HEX_DIGITS[random_bytes[i] >> 4];
    destination[i * 2 + 1] = HEX_DIGITS[random_bytes[i] & 0x0F];
  }
  destination[OTA_SECRET_CHARS] = '\0';
}

bool requireOTAAuthentication(AsyncWebServerRequest* request) {
  if (request->authenticate(OTA_USERNAME, ota_password)) {
    return true;
  }
  request->requestAuthentication();
  return false;
}

bool allowOTAFlashIO() {
#if defined(WITH_MQTT_REPORTER)
  return ota_barrier.allowFlashIO(meshcore_mqtt_can_flash_after_stop());
#else
  return ota_barrier.allowFlashIO(true);
#endif
}

void resumeMQTTAfterOTAAbort() {
#if defined(WITH_MQTT_REPORTER)
  if (!meshcore_mqtt_resume_after_ota_abort()) {
    MESH_DEBUG_PRINTLN("OTA: MQTT restart withheld; previous stop remains unproven");
  }
#endif
}

} // namespace

bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  if (ota_active) {
    snprintf(reply, 160, "Error: OTA already active");
    return false;
  }
  if (ota_server != nullptr) {
    snprintf(reply, 160, "Error: reboot before starting OTA again");
    return false;
  }

  ota_barrier.reset();
  if (!ota_barrier.requestStop()) {
    snprintf(reply, 160, "Error: OTA lifecycle barrier unavailable");
    return false;
  }

  bool mqtt_stop_clean = true;
#if defined(WITH_MQTT_REPORTER)
  mqtt_stop_clean = meshcore_mqtt_stop_for_ota();
#endif
  ota_barrier.onStopComplete(mqtt_stop_clean);
  if (!mqtt_stop_clean) {
    ota_barrier.abort();
    resumeMQTTAfterOTAAbort();
    snprintf(reply, 160, "Error: MQTT stop unverified; OTA flash refused");
    MESH_DEBUG_PRINTLN("startOTAUpdate: MQTT stop unverified; flash gate remains closed");
    return false;
  }

  char ap_password[OTA_SECRET_CHARS + 1];
  generateOTASecret(ap_password);
  generateOTASecret(ota_password);

  ota_previous_wifi_mode = WiFi.getMode();
  ota_previous_inhibit_sleep = inhibit_sleep;
  inhibit_sleep = true;

#if defined(WITH_MQTT_REPORTER)
  // AsyncWebServer listens on every active interface. Stop station networking
  // before opening the listener so OTA cannot be reached from the site LAN.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
#endif
  if (!WiFi.mode(WIFI_AP) || !WiFi.softAP(OTA_AP_SSID, ap_password)) {
    WiFi.mode(ota_previous_wifi_mode);
#if defined(WITH_MQTT_REPORTER)
    WiFi.setAutoReconnect(true);
#endif
    inhibit_sleep = ota_previous_inhibit_sleep;
    ota_barrier.abort();
    resumeMQTTAfterOTAAbort();
    snprintf(reply, 160, "Error: could not start isolated OTA access point");
    return false;
  }

  static char id_buf[60];
  snprintf(id_buf, sizeof(id_buf), "%s (%s)", id, getManufacturerName());
  static char home_buf[90];
  snprintf(home_buf, sizeof(home_buf), "<H2>Hi! I am a MeshCore Repeater. ID: %s</H2>", id);

  ota_server = new AsyncWebServer(80);

  ota_server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireOTAAuthentication(request)) return;
    request->send(200, "text/html", home_buf);
  });
  ota_server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireOTAAuthentication(request)) return;
    request->send(SPIFFS, "/packet_log", "text/plain");
  });
  ota_server->onNotFound([](AsyncWebServerRequest *request) {
    if (!requireOTAAuthentication(request)) return;
    request->send(404, "text/plain", "Not found");
  });

  AsyncElegantOTA.setID(id_buf);
  AsyncElegantOTA.setFlashGate(allowOTAFlashIO);
  // ESP-IDF also verifies image signatures here when Secure Boot is provisioned.
  // OTA-enabled production devices should use Secure Boot for that extra boundary.
  AsyncElegantOTA.begin(ota_server, OTA_USERNAME, ota_password);
  ota_server->begin();

  ota_started_at = millis();
  ota_active = true;
  snprintf(reply, 160, "Started: http://%s/update AP-pass=%s HTTP=%s:%s (%lu min)",
           WiFi.softAPIP().toString().c_str(), ap_password, OTA_USERNAME, ota_password,
           (unsigned long)(OTA_WINDOW_MS / 60000UL));
  MESH_DEBUG_PRINTLN("startOTAUpdate: isolated AP active for %lu ms", (unsigned long)OTA_WINDOW_MS);

  return true;
}

void ESP32Board::serviceOTAUpdate() {
  if (!ota_active || (uint32_t)(millis() - ota_started_at) < OTA_WINDOW_MS) {
    return;
  }

  // Close the cross-task flash gate before touching either the updater or the
  // server so an already-open upload cannot write another chunk during expiry.
  ota_barrier.abort();
  AsyncElegantOTA.abort();
  ota_server->end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(ota_previous_wifi_mode);
#if defined(WITH_MQTT_REPORTER)
  WiFi.setAutoReconnect(true);
#endif
  inhibit_sleep = ota_previous_inhibit_sleep;
  ota_active = false;
  memset(ota_password, 0, sizeof(ota_password));
  resumeMQTTAfterOTAAbort();
  MESH_DEBUG_PRINTLN("serviceOTAUpdate: OTA window expired");
}

bool ESP32Board::isOTAUpdateActive() const {
  return ota_active;
}

#else
bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  return false; // not supported
}

void ESP32Board::serviceOTAUpdate() {
}

bool ESP32Board::isOTAUpdateActive() const {
  return false;
}
#endif

void ESP32Board::powerOff() {
  enterDeepSleep(0); // Do not wakeup
}

void ESP32Board::enterDeepSleep(uint32_t secs) {
  // Power off the display if any
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif

  // Power off LoRa
  radio_driver.powerOff();

  // Keep LoRa inactive during deepsleep
  digitalWrite(P_LORA_NSS, HIGH);
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
  gpio_hold_en((gpio_num_t)P_LORA_NSS);
#else
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
#endif

  // Power off GPS if any
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  // Flush serial buffers
  Serial.flush();
  delay(100);

  // Clear stale wakeup sources to avoid ghost wakeup
  // This is required when Power Management and automatic lightsleep are enabled
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }

  // Finally set ESP32 into deepsleep
  esp_deep_sleep_start(); // CPU halts here and never returns!
}
#endif
