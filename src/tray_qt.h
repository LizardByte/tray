/**
 * @file src/tray_qt.h
 * @brief Internal helpers for the Qt system tray implementation.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

namespace tray_qt {
  /**
   * @brief Select the ordered Qt platform plugin candidates for Linux.
   *
   * An explicit platform selection is preserved. Otherwise, available desktop
   * environment hints determine whether Wayland or X11 is tried first, and the
   * headless minimal plugin is retained as the final fallback.
   *
   * @param requested_platform Explicit QT_QPA_PLATFORM value.
   * @param session_type XDG session type, such as `wayland` or `x11`.
   * @param wayland_display WAYLAND_DISPLAY value.
   * @param x11_display DISPLAY value.
   * @return Semicolon-separated Qt platform plugin candidates.
   */
  std::string select_platform_plugins(
    std::string_view requested_platform,
    std::string_view session_type,
    std::string_view wayland_display,
    std::string_view x11_display
  );
}  // namespace tray_qt
