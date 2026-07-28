/**
 * @file notification_utils.cpp
 * @brief Native notification helper definitions for tests.
 */

// test includes
#include "notification_utils.h"

// standard includes
#include <array>
#include <chrono>
#include <string>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>

#if defined(__linux__)
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char **environ;

namespace {
  void closeFreedesktopNotifications() {
    for (int id = 1; id <= 128; ++id) {
      std::array<std::string, 7> arguments {
        "dbus-send",
        "--session",
        "--print-reply=literal",
        "--dest=org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications.CloseNotification",
        "uint32:" + std::to_string(id),
      };
      std::array<char *, 8> argv {};
      for (std::size_t i = 0; i < arguments.size(); ++i) {
        argv[i] = arguments[i].data();
      }

      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

      pid_t child = 0;
      const int spawn_result = posix_spawnp(&child, arguments[0].c_str(), &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      if (spawn_result != 0) {
        return;
      }
      waitpid(child, nullptr, 0);
    }
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
