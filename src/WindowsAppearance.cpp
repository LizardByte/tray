/**
 * @file src/WindowsAppearance.cpp
 * @brief Definitions for Windows system tray appearance handling.
 */

/**
 * @def WIN32_LEAN_AND_MEAN
 * @brief Exclude rarely used Windows declarations.
 */
#define WIN32_LEAN_AND_MEAN

// standard includes
#include <cstdint>
#include <exception>
#include <optional>

// platform includes
#include <Windows.h>
#include <WtsApi32.h>

// qt includes
#include <QApplication>
#include <QDebug>
#include <QOperatingSystemVersion>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QtGlobal>

// local includes
#include "WindowsAppearance.h"

namespace tray_qt::windows {
  color_scheme_e color_scheme_from_apps_use_light_theme(std::optional<std::uint32_t> apps_use_light_theme) {
    if (!apps_use_light_theme) {
      return color_scheme_e::unknown;
    }

    return *apps_use_light_theme == 0 ? color_scheme_e::dark : color_scheme_e::light;
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  /**
   * @brief Read AppsUseLightTheme from the user currently being impersonated.
   *
   * @return The registry value, or no value when it cannot be read.
   */
  static std::optional<std::uint32_t> current_user_apps_use_light_theme() {
    HKEY user_key = nullptr;
    const auto open_error = RegOpenCurrentUser(KEY_QUERY_VALUE, &user_key);
    if (open_error != ERROR_SUCCESS) {
      return std::nullopt;
    }

    DWORD apps_use_light_theme = 0;
    DWORD value_size = sizeof(apps_use_light_theme);
    const auto query_error = RegGetValueW(
      user_key,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme",
      RRF_RT_REG_DWORD,
      nullptr,
      &apps_use_light_theme,
      &value_size
    );
    RegCloseKey(user_key);

    if (query_error != ERROR_SUCCESS) {
      return std::nullopt;
    }

    return apps_use_light_theme;
  }
#endif

  color_scheme_e interactive_user_color_scheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const auto console_session_id = WTSGetActiveConsoleSessionId();
    HANDLE user_token = nullptr;
    if (console_session_id != 0xFFFFFFFF && WTSQueryUserToken(console_session_id, &user_token)) {
      if (ImpersonateLoggedOnUser(user_token)) {
        const auto apps_use_light_theme = current_user_apps_use_light_theme();
        if (!RevertToSelf()) {
          qCritical() << "QtTrayMenu: failed to revert user impersonation after reading the Windows color scheme:" << GetLastError();
          std::terminate();
        }
        CloseHandle(user_token);
        return color_scheme_from_apps_use_light_theme(apps_use_light_theme);
      }
      CloseHandle(user_token);
    }

    return color_scheme_from_apps_use_light_theme(current_user_apps_use_light_theme());
#else
    return color_scheme_e::unknown;
#endif
  }

  bool should_use_windows_11_style() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
#else
    return false;
#endif
  }

  bool windows_11_style_is_active() {
    return QApplication::instance() != nullptr && QApplication::style()->objectName().compare(QStringLiteral("windows11"), Qt::CaseInsensitive) == 0;
  }

  color_scheme_e current_color_scheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    switch (QApplication::styleHints()->colorScheme()) {
      case Qt::ColorScheme::Light:
        return color_scheme_e::light;
      case Qt::ColorScheme::Dark:
        return color_scheme_e::dark;
      default:
        return color_scheme_e::unknown;
    }
#else
    return color_scheme_e::unknown;
#endif
  }

  void sync_color_scheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const auto color_scheme = interactive_user_color_scheme();
    if (color_scheme != color_scheme_e::unknown) {
      QApplication::styleHints()->setColorScheme(color_scheme == color_scheme_e::dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    } else {
      qWarning("QtTrayMenu: could not read the interactive user's Windows application color scheme");
    }
#endif
  }

  void configure_appearance(QApplication *app) {
    if (app == nullptr) {
      qWarning("QtTrayMenu: cannot configure Windows appearance without a QApplication");
      return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    sync_color_scheme();

    if (should_use_windows_11_style() && !windows_11_style_is_active()) {
      if (auto *windows_11_style = QStyleFactory::create(QStringLiteral("windows11"))) {
        QApplication::setStyle(windows_11_style);
      } else {
        qWarning() << "QtTrayMenu: the Qt Windows 11 style is unavailable; using" << app->style()->objectName();
      }
    }
#else
    qWarning("QtTrayMenu: mirroring the interactive user's color scheme requires Qt 6.8 or newer");
#endif

    qInfo() << "QtTrayMenu: using Qt style" << app->style()->objectName();
  }
}  // namespace tray_qt::windows
