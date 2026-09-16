// SPDX-License-Identifier: MIT
// Helpers for parsing `set mqtt.*` credential arguments from the serial CLI.
// Extracted so the blank-clearing rules stay host-testable (test_mqtt_command).
#pragma once

#include <cstring>

namespace mqtt_credential_set {

// True when the dotted mqtt key names a broker credential field, e.g.
// "username", "password", "1.username", "3.password".
inline bool isCredentialKey(const char *key) {
  if (key == nullptr) {
    return false;
  }
  const char *tail = std::strrchr(key, '.');
  tail = (tail != nullptr) ? tail + 1 : key;
  return std::strcmp(tail, "username") == 0 || std::strcmp(tail, "password") == 0;
}

// A CLI value of exactly two double-quotes means "blank"; rewrite it in place
// and return the pointer. Never returns nullptr.
inline char *normalizeCredentialValue(char *value) {
  static char empty_value[] = "";
  if (value == nullptr) {
    return empty_value;
  }
  if (value[0] == '"' && value[1] == '"' && value[2] == '\0') {
    value[0] = '\0';
  }
  return value;
}

}  // namespace mqtt_credential_set
