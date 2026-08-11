#pragma once

#include <cstddef>
#include <cstdio>

inline void writeMqttReply(char *reply, size_t reply_size, const char *message) {
  if (reply == nullptr || reply_size == 0) return;
  std::snprintf(reply, reply_size, "%s", message ? message : "");
}

inline void writeMqttValueReply(char *reply, size_t reply_size, const char *value) {
  if (reply == nullptr || reply_size == 0) return;
  std::snprintf(reply, reply_size, "> %s", value ? value : "");
}
