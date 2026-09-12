#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

class SerialInputBuffer {
public:
  enum class AppendResult {
    Ignored,
    Appended,
    LineReady,
  };

  SerialInputBuffer(char *buffer, size_t capacity)
      : _buffer(buffer), _capacity(capacity), _length(0),
        _line_ready(false), _discard_until_cr(false) {
    reset();
  }

  void reset() {
    _length = 0;
    _line_ready = false;
    _discard_until_cr = false;
    clearBuffer();
  }

  AppendResult append(char c) {
    if (_buffer == nullptr || _capacity == 0) return AppendResult::Ignored;

    // A full line is reported immediately, but bytes left on that line must
    // not become the start of the next command. Keep consuming until its CR.
    if (_discard_until_cr) {
      if (c == '\r') _discard_until_cr = false;
      return AppendResult::Ignored;
    }

    if (_line_ready) return AppendResult::LineReady;
    if (c == '\n') return AppendResult::Ignored;
    if (c == '\r') {
      _line_ready = true;
      return AppendResult::LineReady;
    }

    // Leave one byte for the NUL terminator. Reaching this boundary
    // deterministically truncates the line and preserves the terminator.
    if (_length >= _capacity - 1) {
      _line_ready = true;
      _discard_until_cr = true;
      return AppendResult::LineReady;
    }

    _buffer[_length++] = c;
    _buffer[_length] = '\0';
    if (_length == _capacity - 1) {
      _line_ready = true;
      _discard_until_cr = true;
      return AppendResult::LineReady;
    }
    return AppendResult::Appended;
  }

  // Clear the completed command. If the line was full, retain the discard
  // state so a pending terminator cannot produce a spurious empty command.
  void finishLine() {
    if (_discard_until_cr) {
      _length = 0;
      _line_ready = false;
      clearBuffer();
      return;
    }
    reset();
  }

  char *data() { return _buffer; }
  const char *data() const { return _buffer; }
  size_t size() const { return _length; }
  bool lineReady() const { return _line_ready; }

private:
  void clearBuffer() {
    if (_buffer != nullptr && _capacity > 0) _buffer[0] = '\0';
  }

  char *_buffer;
  size_t _capacity;
  size_t _length;
  bool _line_ready;
  bool _discard_until_cr;
};

namespace cli_input {

inline bool isSecretCommandInput(const char *line) {
  if (line == nullptr) return false;
  if (strncmp(line, "password ", 9) == 0 ||
      strncmp(line, "guest.password ", 15) == 0 ||
      strncmp(line, "set guest.password ", 19) == 0 ||
      strncmp(line, "set bridge.secret ", 18) == 0) {
    return true;
  }
  if (strncmp(line, "set mqtt.", 9) != 0) return false;

  const char *key = line + 9;
  const char *value = strchr(key, ' ');
  if (value == nullptr) return false;
  size_t key_len = static_cast<size_t>(value - key);
  if (key_len == 9 && strncmp(key, "wifi.pass", key_len) == 0) return true;
  if (key_len == 8 && strncmp(key, "password", key_len) == 0) return true;
  return key_len >= 9 && strncmp(value - 9, ".password", 9) == 0;
}

inline bool parseAirtimeFactor(const char *text, float &value) {
  if (text == nullptr) return false;

  errno = 0;
  char *end = nullptr;
  const float parsed = strtof(text, &end);
  if (end == text) return false;

  // atof() accepted surrounding whitespace, so preserve that valid syntax,
  // while rejecting any other trailing characters.
  while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end != '\0' || errno == ERANGE || !isfinite(parsed) ||
      parsed < 0.0f || parsed > 9.0f) {
    return false;
  }

  value = parsed;
  return true;
}

}  // namespace cli_input
