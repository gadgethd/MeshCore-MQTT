#pragma once

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <WiFi.h>
#include <mqtt_client.h>
#include "MqttSettings.h"

class MyMesh;

#ifndef LORA_FREQ
  #define LORA_FREQ 869.525
#endif

#ifndef LORA_BW
  #define LORA_BW 62.5
#endif

#ifndef LORA_SF
  #define LORA_SF 8
#endif

#ifndef LORA_CR
  #define LORA_CR 5
#endif

class MqttReporter {
public:
  MqttReporter(MyMesh &mesh, mesh::RTCClock &clock);
  ~MqttReporter();

  void begin(FILESYSTEM *fs);
  void loop();

  void publishRxRaw(const uint8_t raw[], int len);
  void publishRxPacket(mesh::Packet *pkt, int len, float score, int rssi, float snr, uint32_t duration_ms);
  void publishTxPacket(mesh::Packet *pkt, int len);

  const char *getWiFiSsid() const;
  bool isWiFiConnected() const;
  bool isMqttConnected() const;
  bool getConfigValue(const char *key, char *dest, size_t dest_size, bool mask_secret = false) const;
  bool setConfigValue(const char *key, const char *value);
  bool resetConfig();
  void reconnect();
  void printConfig(Print &out) const;

private:
  MyMesh *_mesh;
  mesh::RTCClock *_clock;
  esp_mqtt_client_handle_t _mqtt_client;
  MqttSettingsStore _settings;
  String _last_rx_raw;
  String _offline_payload;
  String _radio_string;
  unsigned long _last_wifi_attempt;
  unsigned long _last_status_publish;
  bool _time_synced;
  bool _mqtt_started;
  bool _mqtt_connected;
  char _origin_id[65];
  char _client_id[40];
  char _status_topic[128];
  char _packets_topic[128];

  void ensureIdentityStrings();
  void resetConnections();
  bool connectWiFi();
  bool connectMQTT();
  void syncTimeFromNtp();
  void publishStatus(const char *status);
  void handleMqttEvent(esp_mqtt_event_handle_t event);
  bool mqttNeedsTimeSync() const;

  String buildIsoTimestamp() const;
  String buildTimeField() const;
  String buildDateField() const;
  String buildRadioString() const;
  String buildStatusPayload(const char *status) const;
  String buildPacketPayload(
      const char *direction,
      mesh::Packet *pkt,
      int len,
      const String &raw_hex,
      float score,
      int rssi,
      float snr,
      uint32_t duration_ms,
      bool include_radio_metrics) const;

  static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event);
  static String jsonEscape(const char *input);
  static String bytesToHex(const uint8_t *data, size_t len);
  static bool shouldIncludePath(const mesh::Packet *pkt);
};

#endif
