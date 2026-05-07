/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

// Qt includes
#include <QString>

namespace {
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  void (*g_log_cb)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * @brief Qt message handler that forwards to the registered log callback.
   * @param type The Qt message type.
   * @param msg The message string.
   */
  void qt_message_handler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    if (g_log_cb == nullptr) {
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
    g_log_cb(level, msg.toUtf8().constData());
  }
}  // namespace

extern "C" {
  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    const auto app_name_ = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    const auto app_display_name_ = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    const auto desktop_name_ = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();
    qt_tray_menu->configureAppMetadata(app_name_, app_display_name_, desktop_name_);
  }

  int tray_init(struct tray *tray) {
    if (qt_tray_menu == nullptr) {
      qt_tray_menu = std::make_unique<QtTrayMenu>();
    }
    return qt_tray_menu->init(tray);
  }

  int tray_loop(int blocking) {
    return qt_tray_menu->loop(blocking);
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995) - C API requires this exact mutable-pointer signature
    qt_tray_menu->update(tray);
  }

  void tray_exit(void) {
    qt_tray_menu->exit();
    qt_tray_menu.reset();
    qt_tray_menu = nullptr;
  }

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {  // NOSONAR(cpp:S5205) - C API requires a plain function pointer callback type
    g_log_cb = cb;
    if (cb != nullptr) {
      qInstallMessageHandler(qt_message_handler);
    } else {
      qInstallMessageHandler(nullptr);
    }
  }

  void tray_show_menu(void) {
    qt_tray_menu->showMenu();
  }

  void tray_simulate_menu_item_click(int index) {
    qt_tray_menu->clickMenuItem(index);
  }

  void tray_simulate_notification_click(void) {
    qt_tray_menu->clickMessage();
  }
}  // extern "C"
