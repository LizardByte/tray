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
#include <QMetaObject>
#include <QString>
#include <QThread>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

namespace tray_qt {
  /**
   * @brief Process-wide state backing the C tray API.
   */
  struct State {
    std::unique_ptr<QtTrayMenu> trayMenu;  ///< Active tray menu instance.
    void (*logCallback)(int, const char *) = nullptr;  ///< Registered C logging callback.
    bool appInfoConfigured = false;  ///< Whether application metadata was explicitly configured.
    QString appName;  ///< Configured application name.
    QString appDisplayName;  ///< Configured application display name.
    QString desktopName;  ///< Configured desktop file name.
  };

  /**
   * @brief Access the process-wide tray API state.
   * @return Mutable tray API state.
   */
  State &state() {
    static State instance;
    return instance;
  }

  /**
   * @brief Acknowledge/click current notification.
   */
  void acknowledge_notification() {
    if (state().trayMenu != nullptr && QtTrayMenu::supportsMessages()) {
      state().trayMenu->clickMessage();
    }
  }

  /**
   * @brief Clear current notification state without invoking callbacks.
   */
  void clear_notification() {
    if (state().trayMenu != nullptr) {
      state().trayMenu->clearMessageCallback();
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
    if (state().trayMenu != nullptr && QtTrayMenu::supportsMessages()) {
      if (tray->notification_icon != nullptr) {
        state().trayMenu->showMessage(tray->notification_title, tray->notification_text, tray->notification_icon, tray->notification_cb);
      } else {
        state().trayMenu->showMessage(tray->notification_title, tray->notification_text, tray->notification_cb);
      }
    }
  }

  /**
   * @brief Apply configured Qt application metadata to the active Qt tray menu.
   * @param allow_defaults Whether empty app info values should apply fallback defaults.
   */
  void apply_app_info(const bool allow_defaults = true) {
    const auto &current_state = state();
    if (!current_state.appInfoConfigured || current_state.trayMenu == nullptr) {
      return;
    }
    if (!allow_defaults && current_state.appName.isEmpty() && current_state.appDisplayName.isEmpty()) {
      return;
    }

    current_state.trayMenu->configureAppMetadata(current_state.appName, current_state.appDisplayName, current_state.desktopName);
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
    if (state().logCallback == nullptr) {
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
    state().logCallback(level, msg.toUtf8().constData());
  }
}  // namespace tray_qt

extern "C" {
  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    auto &state = tray_qt::state();
    state.appInfoConfigured = true;
    state.appName = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    state.appDisplayName = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    state.desktopName = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();

    tray_qt::apply_app_info();
  }

  int tray_init(struct tray *tray) {
    auto &state = tray_qt::state();
    if (state.trayMenu == nullptr) {
      tray_qt::configure_platform();
      // Create a new unique pointer to QtTrayMenu instance
      state.trayMenu = std::make_unique<QtTrayMenu>();
      tray_qt::apply_app_info(false);
    }

    if (const auto result = state.trayMenu->init(tray, false); result < 0) {
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
    if (tray_qt::state().trayMenu == nullptr) {
      return -1;
    }
    return tray_qt::state().trayMenu->loop(blocking);
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995): C API requires this exact mutable-pointer signature
    if (tray_qt::state().trayMenu == nullptr) {
      return;
    }

    auto *const tray_menu = tray_qt::state().trayMenu.get();
    const auto apply_update = [tray_menu, tray]() {
      tray_menu->update(tray, false);
      tray_qt::notify(tray);
    };

    if (QThread::currentThread() == tray_menu->thread()) {
      apply_update();
      return;
    }

    // Keep the C API synchronous so callers can safely reuse or release tray data after this function returns.
    (void) QMetaObject::invokeMethod(tray_menu, apply_update, Qt::BlockingQueuedConnection);
  }

  void tray_exit(void) {
    if (tray_qt::state().trayMenu == nullptr) {
      return;
    }
    tray_qt::state().trayMenu->exit();
  }

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {  // NOSONAR(cpp:S5205): C API requires a plain function pointer callback type
    tray_qt::state().logCallback = cb;
    if (cb != nullptr) {
      qInstallMessageHandler(tray_qt::qt_message_handler);
    } else {
      qInstallMessageHandler(nullptr);
    }
  }

  void tray_show_menu(void) {
    if (tray_qt::state().trayMenu == nullptr) {
      return;
    }
    tray_qt::state().trayMenu->showMenu();
  }

  int tray_position_mouse_over_icon(void) {
    if (tray_qt::state().trayMenu == nullptr) {
      return -1;
    }
    return tray_qt::state().trayMenu->positionMouseOverIcon() ? 0 : -1;
  }

  int tray_restore_mouse_position(void) {
    if (tray_qt::state().trayMenu == nullptr) {
      return -1;
    }
    return tray_qt::state().trayMenu->restoreMousePosition() ? 0 : -1;
  }

  void tray_simulate_menu_item_click(int index) {
    if (tray_qt::state().trayMenu == nullptr) {
      return;
    }
    tray_qt::state().trayMenu->clickMenuItem(index);
  }

  void tray_simulate_notification_click(void) {
    tray_qt::acknowledge_notification();
  }

}  // extern "C"
