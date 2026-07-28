/**
 * @file notification_utils.cpp
 * @brief Native notification helper definitions for tests.
 */

// test includes
#include "notification_utils.h"

// standard includes
#include <chrono>
#include <cstdlib>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>

#if defined(__linux__)
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
    (void) std::system(close_notifications);  // NOSONAR(cpp:S4721): test-only cleanup of desktop notifications
  }
}  // namespace
#endif

void dismissNativeNotifications() {
#if defined(__linux__)
  closeFreedesktopNotifications();
  constexpr auto wait_timeout = std::chrono::milliseconds(500);
  std::this_thread::sleep_for(wait_timeout);
#endif
}

void waitForNativeNotificationTimeout() {
#if defined(_WIN32)
  if (!lizardbyte::common::is_github_actions()) {
    return;
  }

  constexpr auto wait_timeout = std::chrono::milliseconds(6000);
  std::this_thread::sleep_for(wait_timeout);
#elif defined(__linux__)
  dismissNativeNotifications();
#endif
}
