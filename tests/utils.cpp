/**
 * @file utils.cpp
 * @brief Utility functions
 */
// test includes
#include "utils.h"

// standard includes
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef __linux__
namespace {
  void closeFreedesktopNotifications() {
    constexpr const char *close_notifications =
      "if command -v dbus-send >/dev/null 2>&1; then "
      "id=1; while [ \"$id\" -le 128 ]; do "
      "dbus-send --session --print-reply=literal --dest=org.freedesktop.Notifications "
      "/org/freedesktop/Notifications org.freedesktop.Notifications.CloseNotification uint32:$id "
      ">/dev/null 2>&1; "
      "id=$((id + 1)); "
      "done; "
      "fi";
    (void) std::system(close_notifications);  // NOSONAR(cpp:S4721) - test-only cleanup of desktop notifications
  }
}  // namespace
#endif

/**
 * @brief Set an environment variable.
 * @param name Name of the environment variable
 * @param value Value of the environment variable
 * @return 0 on success, non-zero error code on failure
 */
int setEnv(const std::string &name, const std::string &value) {
#ifdef _WIN32
  return _putenv_s(name.c_str(), value.c_str());
#else
  return setenv(name.c_str(), value.c_str(), 1);
#endif
}

void dismissNativeNotifications() {
#ifdef __linux__
  closeFreedesktopNotifications();
  constexpr auto wait_timeout = std::chrono::milliseconds(500);
  std::this_thread::sleep_for(wait_timeout);
#endif
}

void waitForNativeNotificationTimeout() {
#ifdef _WIN32
  constexpr auto wait_timeout = std::chrono::milliseconds(6000);
  std::this_thread::sleep_for(wait_timeout);
#elif defined(__linux__)
  dismissNativeNotifications();
#endif
}
