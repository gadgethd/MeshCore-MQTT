#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The live-slot accounting shape is adapted from the MIT-licensed
// agessaman/MeshCore observer-firmware MQTT bridge.  This header deliberately
// keeps the decision independent from esp-mqtt and Arduino so it can be tested
// without a board or a live client.
namespace mqtt_tls_cap {

enum class Transport : uint8_t {
  Plain = 0,
  Tls,
  Unknown,
};

enum class ClientState : uint8_t {
  None = 0,
  Connecting,
  Connected,
  Disconnected,
};

struct Slot {
  // This is the current stored setting. It is intentionally not used when a
  // client is already allocated: effective_transport is the live resource.
  bool configured_enabled;
  ClientState client_state;
  Transport effective_transport;
};

inline Transport transportForUri(const char *uri) {
  if (uri == nullptr) return Transport::Unknown;
  if (strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "ws://", 5) == 0) {
    return Transport::Plain;
  }
  if (strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0) {
    return Transport::Tls;
  }
  return Transport::Unknown;
}

inline bool countsAgainstTlsBudget(Transport transport) {
  // Unknown schemes are charged conservatively. A future transport must not
  // bypass the TLS heap guard merely because this build does not recognize it.
  return transport != Transport::Plain;
}

inline const char *transportName(Transport transport) {
  switch (transport) {
    case Transport::Plain: return "plain";
    case Transport::Tls: return "tls";
    case Transport::Unknown: return "unknown";
    default: return "unknown";
  }
}

inline bool isLive(const Slot &slot) {
  return slot.client_state != ClientState::None;
}

inline uint8_t maxActiveTls(bool psram_found, uint8_t no_psram_limit,
                            uint8_t psram_limit) {
  return psram_found ? psram_limit : no_psram_limit;
}

inline uint8_t liveTlsCount(const Slot *slots, size_t slot_count) {
  if (slots == nullptr) return 0;
  uint8_t count = 0;
  for (size_t i = 0; i < slot_count; ++i) {
    if (isLive(slots[i]) && countsAgainstTlsBudget(slots[i].effective_transport) &&
        count < UINT8_MAX) {
      ++count;
    }
  }
  return count;
}

inline bool canStart(Transport requested, const Slot *slots, size_t slot_count,
                     bool psram_found, uint8_t no_psram_limit,
                     uint8_t psram_limit) {
  if (!countsAgainstTlsBudget(requested)) return true;
  return liveTlsCount(slots, slot_count) <
         maxActiveTls(psram_found, no_psram_limit, psram_limit);
}

// Lifecycle helpers make the accounting rule explicit at each SDK boundary.
// A disconnected client still owns its transport until it is destroyed.
inline void recordClientCreated(Slot &slot, Transport transport) {
  slot.client_state = ClientState::Connecting;
  slot.effective_transport = transport;
}

inline void recordClientConnected(Slot &slot) {
  if (slot.client_state != ClientState::None) slot.client_state = ClientState::Connected;
}

inline void recordClientDisconnected(Slot &slot) {
  if (slot.client_state != ClientState::None) slot.client_state = ClientState::Disconnected;
}

inline void recordClientDestroyed(Slot &slot) {
  slot.client_state = ClientState::None;
  slot.effective_transport = Transport::Unknown;
}

}  // namespace mqtt_tls_cap
