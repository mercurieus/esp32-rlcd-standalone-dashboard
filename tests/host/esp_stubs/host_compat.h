// POSIX calls the firmware sources use that MinGW-w64 does not declare.
//
// Force-included into host_tests only (tests/host/CMakeLists.txt), so neither
// components/net_time/net_time_convert.cpp nor the tests change for the sake
// of one host toolchain. On Linux/macOS this header is empty.
#pragma once

#ifdef _WIN32
#include <stdlib.h>
#include <time.h>

// setenv(name, value, overwrite) via _putenv_s. _putenv_s always overwrites,
// so honour overwrite == 0 by leaving an existing variable alone.
static inline int setenv(const char* name, const char* value, int overwrite) {
  if (!overwrite && getenv(name) != NULL) {
    return 0;
  }
  return _putenv_s(name, value) == 0 ? 0 : -1;
}

// localtime_r(timer, result): localtime_s takes the same two pointers in the
// opposite order and returns an errno_t instead of the result pointer.
static inline struct tm* localtime_r(const time_t* timer, struct tm* result) {
  return localtime_s(result, timer) == 0 ? result : NULL;
}
#endif
