/**
 * @file src/WindowsAppearance.h
 * @brief Declarations for Windows system tray appearance handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>

class QApplication;

namespace tray_qt::windows {
  /**
   * @brief Color scheme requested for the Windows system tray.
   */
  enum class color_scheme_e {
    unknown,  ///< No application color scheme preference is available.
    light,  ///< Use the light application color scheme.
    dark  ///< Use the dark application color scheme.
  };

  /**
   * @brief Convert the Windows AppsUseLightTheme setting to a tray color scheme.
   *
   * @param apps_use_light_theme The registry setting, or no value when it is unavailable.
   * @return The corresponding tray color scheme.
   */
  color_scheme_e color_scheme_from_apps_use_light_theme(std::optional<std::uint32_t> apps_use_light_theme);

  /**
   * @brief Read the interactive user's Windows application color scheme.
   *
   * @return The requested color scheme, or unknown when it cannot be read.
   */
  color_scheme_e interactive_user_color_scheme();

  /**
   * @brief Check whether the modern Windows 11 Qt style should be used.
   *
   * @return true when the operating system and Qt version support the style.
   */
  bool should_use_windows_11_style();

  /**
   * @brief Check whether the modern Windows 11 Qt style is active.
   *
   * @return true when QApplication is using the windows11 style.
   */
  bool windows_11_style_is_active();

  /**
   * @brief Read the color scheme currently applied to Qt.
   *
   * @return The active Qt color scheme, or unknown when the Qt version does not expose it.
   */
  color_scheme_e current_color_scheme();

  /**
   * @brief Refresh Qt's color scheme from the interactive user's Windows preference.
   */
  void sync_color_scheme();

  /**
   * @brief Apply the interactive user's color scheme and the modern Windows style to Qt.
   *
   * @param app The tray QApplication instance.
   */
  void configure_appearance(const QApplication *app);
}  // namespace tray_qt::windows
