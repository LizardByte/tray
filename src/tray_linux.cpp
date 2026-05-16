/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// local includes
#include "tray.h"

// lib includes
#include <libnotify/notify.h>

// Qt includes
#include "QtTrayMenu.h"

#include <QString>

namespace tray_linux {
  NotifyNotification *notificationCurrent = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  void (*notificationCurrentCallback)() = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  std::mutex notificationMutex;  // NOSONAR(cpp:S5421) - mutable state, not const
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  void (*log_cb)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * @brief Initialize notifications
   */
  void init_notify() {
    if (!notify_is_initted()) {
      std::scoped_lock lock(notificationMutex);
      notify_init("tray");
    }
  }

  /**
   * @brief Uninitialize notifications
   * @param app_name the current application name
   */
  void set_notify_app_info(const char *app_name) {
    std::scoped_lock lock(notificationMutex);
    if (app_name) {
      notify_set_app_name(app_name);
    }
  }

  /**
   * @brief Handle tray notifications via desktop-independent interface
   * @param tray Tray structure containing notification information
   * @return true if notified successfully, false otherwise
   */
  void notify(struct tray *tray) {
    if (tray->notification_text == nullptr || std::string(tray->notification_text).empty()) {
      return;
    }
    // Try to notify using libnotify
    if (notify_is_initted()) {
      std::scoped_lock lock(notificationMutex);
      if (notificationCurrent != nullptr && NOTIFY_IS_NOTIFICATION(notificationCurrent)) {
        notify_notification_close(notificationCurrent, nullptr);
        g_object_unref(G_OBJECT(notificationCurrent));
        notificationCurrent = nullptr;
        notificationCurrentCallback = nullptr;
      }
      const char *notification_icon = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
      notificationCurrent = notify_notification_new(tray->notification_title, tray->notification_text, notification_icon);
      if (notificationCurrent != nullptr && NOTIFY_IS_NOTIFICATION(notificationCurrent)) {
        if (tray->notification_cb != nullptr) {
          notificationCurrentCallback = tray->notification_cb;
          notify_notification_add_action(notificationCurrent, "default", "Default", NOTIFY_ACTION_CALLBACK(tray->notification_cb), nullptr, nullptr);
        }
        if (notify_notification_show(notificationCurrent, nullptr)) {
          return;
        }
      }
    }
    // Fall back to QtTrayMenu notification
    if (qt_tray_menu != nullptr) {
      qt_tray_menu->showMessage(tray->notification_title, tray->notification_text, tray->notification_icon, tray->notification_cb);
    }
  }

  /**
   * @brief Acknowledge/click current notification
   */
  void notify_acknowledge(bool run_callback = false) {
    if (notify_is_initted()) {
      std::scoped_lock lock(notificationMutex);
      if (notificationCurrent != nullptr && NOTIFY_IS_NOTIFICATION(notificationCurrent)) {
        if (run_callback && notificationCurrentCallback != nullptr) {
          notificationCurrentCallback();
        }
        notify_notification_close(notificationCurrent, nullptr);
        g_object_unref(G_OBJECT(notificationCurrent));
        notificationCurrent = nullptr;
        notificationCurrentCallback = nullptr;
      }
    } else if (qt_tray_menu != nullptr) {
      qt_tray_menu->clickMessage();
    }
  }

  /**
   * @brief Uninitialize notifications
   */
  void uninit_notify() {
    if (notify_is_initted()) {
      notify_acknowledge();
      std::scoped_lock lock(notificationMutex);
      notify_uninit();
    }
  }

  /**
   * @brief Qt message handler that forwards to the registered log callback.
   * @param type The Qt message type.
   * @param msg The message string.
   */
  void qt_message_handler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    if (log_cb == nullptr) {
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
    log_cb(level, msg.toUtf8().constData());
  }
}  // namespace tray_linux

extern "C" {
  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    tray_linux::set_notify_app_info(app_name);

    if (tray_linux::qt_tray_menu == nullptr) {
      return;
    }
    const auto app_name_ = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    const auto app_display_name_ = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    const auto desktop_name_ = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();
    tray_linux::qt_tray_menu->configureAppMetadata(app_name_, app_display_name_, desktop_name_);
  }

  int tray_init(struct tray *tray) {
    if (tray_linux::qt_tray_menu == nullptr) {
      tray_linux::qt_tray_menu = std::make_unique<QtTrayMenu>();
    }

    // Init tray menu
    if (const auto result = tray_linux::qt_tray_menu->init(tray, false); result < 0) {
      return result;
    }

    // Init notify
    tray_linux::init_notify();
    tray_linux::notify(tray);
    return 0;
  }

  int tray_loop(int blocking) {
    if (tray_linux::qt_tray_menu == nullptr) {
      return -1;
    }
    return tray_linux::qt_tray_menu->loop(blocking);
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995) - C API requires this exact mutable-pointer signature
    if (tray_linux::qt_tray_menu == nullptr) {
      return;
    }
    tray_linux::qt_tray_menu->update(tray, false);
    tray_linux::notify(tray);
  }

  void tray_exit(void) {
    tray_linux::uninit_notify();

    if (tray_linux::qt_tray_menu == nullptr) {
      return;
    }
    tray_linux::qt_tray_menu->exit();
  }

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {  // NOSONAR(cpp:S5205) - C API requires a plain function pointer callback type
    tray_linux::log_cb = cb;
    if (cb != nullptr) {
      qInstallMessageHandler(tray_linux::qt_message_handler);
    } else {
      qInstallMessageHandler(nullptr);
    }
  }

  void tray_show_menu(void) {
    if (tray_linux::qt_tray_menu == nullptr) {
      return;
    }
    tray_linux::qt_tray_menu->showMenu();
  }

  void tray_simulate_menu_item_click(int index) {
    if (tray_linux::qt_tray_menu == nullptr) {
      return;
    }
    tray_linux::qt_tray_menu->clickMenuItem(index);
  }

  void tray_simulate_notification_click(void) {
    tray_linux::notify_acknowledge(true);
  }
}  // extern "C"
