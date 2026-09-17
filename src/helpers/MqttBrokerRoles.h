// SPDX-License-Identifier: MIT
// Pure MQTT broker-family role classification for status/packet split pairs.
#pragma once

#include <stddef.h>
#include <string.h>

namespace mqtt_broker_roles {

struct BrokerEntry {
  bool enabled;
  const char *effective_root;
  const char *uri;
  const char *iata;
};

struct Role {
  bool status_only;
  bool status_suppressed;
};

inline bool endsWithStatus(const char *root) {
  if (root == nullptr) return false;
  const size_t suffix_length = sizeof("/status") - 1;
  const size_t root_length = strlen(root);
  return root_length >= suffix_length &&
         strcmp(root + root_length - suffix_length, "/status") == 0;
}

inline bool sameText(const char *left, const char *right) {
  if (left == nullptr) return right == nullptr || right[0] == '\0';
  if (right == nullptr) return left[0] == '\0';
  return strcmp(left, right) == 0;
}

inline void classify(const BrokerEntry *entries, size_t entry_count, Role *roles) {
  if (roles == nullptr) return;
  for (size_t i = 0; i < entry_count; ++i) {
    roles[i] = Role{false, false};
  }
  if (entries == nullptr) return;

  for (size_t i = 0; i < entry_count; ++i) {
    roles[i].status_only = entries[i].enabled && endsWithStatus(entries[i].effective_root);
  }

  for (size_t i = 0; i < entry_count; ++i) {
    if (!entries[i].enabled || roles[i].status_only) continue;
    for (size_t j = 0; j < entry_count; ++j) {
      if (!roles[j].status_only || !entries[j].enabled) continue;
      if (sameText(entries[i].uri, entries[j].uri) &&
          sameText(entries[i].iata, entries[j].iata)) {
        roles[i].status_suppressed = true;
        break;
      }
    }
  }
}

}  // namespace mqtt_broker_roles
