/**
 * @file src/tray_qt.cpp
 * @brief System tray implementation using Qt.
 */
// standard includes
#include <memory>

// qt includes
#include <QByteArray>
#include <QDebug>
#include <QMessageLogContext>
#include <QString>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

namespace tray_qt {
  /**
   * QtTrayMenu instance
   */
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Logging callback for qt_message_handler
   */
  void (*log_callback)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Explicit Qt application metadata configured through the C API.
   */
  bool app_info_configured = false;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Qt application name configured through the C API.
   */
  QString app_name;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Qt application display name configured through the C API.
   */
  QString app_display_name;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Qt desktop file name configured through the C API.
   */
  QString desktop_name;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * @brief Acknowledge/click current notification.
   */
  void acknowledge_notification() {
    if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      qt_tray_menu->clickMessage();
    }
  }

  /**
   * @brief Clear current notification state without invoking callbacks.
   */
  void clear_notification() {
    if (qt_tray_menu != nullptr) {
      qt_tray_menu->clearMessageCallback();
    }
  }

  /**
   * @brief Show tray notification via desktop-independent interface
   * @param tray Tray structure containing notification information
   */
  void notify(struct tray *tray) {
    if (tray->notification_text == nullptr || tray->notification_text[0] == '\0') {
      clear_notification();
      return;
    }
    if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      if (tray->notification_icon != nullptr) {
        qt_tray_menu->showMessage(tray->notification_title, tray->notification_text, tray->notification_icon, tray->notification_cb);
      } else {
        qt_tray_menu->showMessage(tray->notification_title, tray->notification_text, tray->notification_cb);
      }
    }
  }

  /**
   * @brief Apply configured Qt application metadata to the active Qt tray menu.
   * @param allow_defaults Whether empty app info values should apply fallback defaults.
   */
  void apply_app_info(const bool allow_defaults = true) {
    if (!app_info_configured || qt_tray_menu == nullptr) {
      return;
    }
    if (!allow_defaults && app_name.isEmpty() && app_display_name.isEmpty()) {
      return;
    }

    qt_tray_menu->configureAppMetadata(app_name, app_display_name, desktop_name);
  }

  /**
   * @brief Configure Linux headless fallback for Qt.
   */
  void configure_platform() {
#if defined(__linux__)
    // Check if a (wayland_)display is set or fallback to minimal QPA platform
    if (qgetenv("WAYLAND_DISPLAY").isEmpty() && qgetenv("DISPLAY").isEmpty()) {
      // Force fallback to QT platform minimal if no (WAYLAND_)DISPLAY was found
      qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("minimal"));
      qWarning("QtTrayMenu: no reachable WAYLAND_DISPLAY or DISPLAY endpoint, forcing QT_QPA_PLATFORM=minimal");
    }
#endif
  }

  /**
   * @brief Qt message handler that forwards to the registered log callback.
   * @param type The Qt message type.
   * @param msg The message string.
   */
  void qt_message_handler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    if (log_callback == nullptr) {
      return;
    }
    int level;
    switch (type) {
      case QtDebugMsg:
        level = 0;
        break;
      case QtInfoMsg:
        level = 1;
        break;
      case QtWarningMsg:
        level = 2;
        break;
      default:
        level = 3;
        break;
    }
    log_callback(level, msg.toUtf8().constData());
  }
}  // namespace tray_qt

extern "C" {
  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    tray_qt::app_info_configured = true;
    tray_qt::app_name = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    tray_qt::app_display_name = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    tray_qt::desktop_name = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();

    tray_qt::apply_app_info();
  }

  int tray_init(struct tray *tray) {
    if (tray_qt::qt_tray_menu == nullptr) {
      tray_qt::configure_platform();
      // Create a new unique pointer to QtTrayMenu instance
      tray_qt::qt_tray_menu = std::make_unique<QtTrayMenu>();
      tray_qt::apply_app_info(false);
    }

    if (const auto result = tray_qt::qt_tray_menu->init(tray, false); result < 0) {
      // Tray init failed. Clean up and return error.
      tray_exit();
      return result;
    }
    tray_qt::apply_app_info();

    if (!QtTrayMenu::supportsMessages()) {
      // Notification support is unavailable. Clean up and return error.
      tray_exit();
      return -1;
    }

    // Fire notification if there is one
    tray_qt::notify(tray);
    return 0;
  }

  int tray_loop(int blocking) {
    if (tray_qt::qt_tray_menu == nullptr) {
      return -1;
    }
    return tray_qt::qt_tray_menu->loop(blocking);
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995) - C API requires this exact mutable-pointer signature
    if (tray_qt::qt_tray_menu == nullptr) {
      return;
    }
    tray_qt::qt_tray_menu->update(tray, false);
    tray_qt::notify(tray);
  }

  void tray_exit(void) {
    if (tray_qt::qt_tray_menu == nullptr) {
      return;
    }
    tray_qt::qt_tray_menu->exit();
  }

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {  // NOSONAR(cpp:S5205) - C API requires a plain function pointer callback type
    tray_qt::log_callback = cb;
    if (cb != nullptr) {
      qInstallMessageHandler(tray_qt::qt_message_handler);
    } else {
      qInstallMessageHandler(nullptr);
    }
  }

  void tray_show_menu(void) {
    if (tray_qt::qt_tray_menu == nullptr) {
      return;
    }
    tray_qt::qt_tray_menu->showMenu();
  }

  void tray_simulate_menu_item_click(int index) {
    if (tray_qt::qt_tray_menu == nullptr) {
      return;
    }
    tray_qt::qt_tray_menu->clickMenuItem(index);
  }

  void tray_simulate_notification_click(void) {
    tray_qt::acknowledge_notification();
  }

}  // extern "C"
