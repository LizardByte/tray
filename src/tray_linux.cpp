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
#include "tray.h"

// lib includes
#include <libnotify/notify.h>

// Qt includes
#include "QtTrayMenu.h"

#include <QString>

namespace {
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  void (*g_log_cb)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  NotifyNotification *currentNotification = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const

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

  /**
   * @brief Handle tray notifications via desktop-independent interface
   * @param tray Tray structure containing notification information
   * @return true if notified successfully, false otherwise
   */
  bool tray_linux_notify(struct tray *tray) {
    if (tray->notification_text != nullptr && strlen(tray->notification_text) > 0 && notify_is_initted()) {
      if (currentNotification != nullptr && NOTIFY_IS_NOTIFICATION(currentNotification)) {
        notify_notification_close(currentNotification, nullptr);
        g_object_unref(G_OBJECT(currentNotification));
      }
      const char *notification_icon = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
      currentNotification = notify_notification_new(tray->notification_title, tray->notification_text, notification_icon);
      if (currentNotification != nullptr && NOTIFY_IS_NOTIFICATION(currentNotification)) {
        if (tray->notification_cb != nullptr) {
          notify_notification_add_action(currentNotification, "default", "Default", NOTIFY_ACTION_CALLBACK(tray->notification_cb), nullptr, nullptr);
        }
        notify_notification_show(currentNotification, nullptr);
        return true;
      }
    }
    return false;
  }
}  // namespace

extern "C" {
  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    if (qt_tray_menu == nullptr) {
      return;
    }
    const auto app_name_ = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    const auto app_display_name_ = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    const auto desktop_name_ = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();
    qt_tray_menu->configureAppMetadata(app_name_, app_display_name_, desktop_name_);
    if (app_name) {
      notify_set_app_name(app_name);
    }
  }

  int tray_init(struct tray *tray) {
    if (qt_tray_menu == nullptr) {
      qt_tray_menu = std::make_unique<QtTrayMenu>();
    }
    if (!notify_is_initted()) {
      notify_init("tray");
    }
    const bool notified = tray_linux_notify(tray);
    return qt_tray_menu->init(tray, !notified);
  }

  int tray_loop(int blocking) {
    if (qt_tray_menu == nullptr) {
      return -1;
    }
    return qt_tray_menu->loop(blocking);
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995) - C API requires this exact mutable-pointer signature
    if (qt_tray_menu == nullptr) {
      return;
    }
    const bool notified = tray_linux_notify(tray);
    qt_tray_menu->update(tray, !notified);
  }

  void tray_exit(void) {
    if (qt_tray_menu == nullptr) {
      return;
    }
    if (notify_is_initted()) {
      notify_uninit();
    }
    qt_tray_menu->exit();
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
    if (qt_tray_menu == nullptr) {
      return;
    }
    qt_tray_menu->showMenu();
  }

  void tray_simulate_menu_item_click(int index) {
    if (qt_tray_menu == nullptr) {
      return;
    }
    qt_tray_menu->clickMenuItem(index);
  }

  void tray_simulate_notification_click(void) {
    if (qt_tray_menu == nullptr) {
      return;
    }
    qt_tray_menu->clickMessage();
  }
}  // extern "C"
