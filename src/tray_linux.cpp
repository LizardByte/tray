/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// lib includes
#include <libnotify/notify.h>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

namespace tray_linux {
  /**
   * Currently shown notification object
   */
  NotifyNotification *notification_current = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Currently shown notification callback
   */
  void (*notification_current_callback)() = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Lock for currently shown notification/callback
   */
  std::mutex notification_mutex;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * QtTrayMenu instance
   */
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Logging callback for qt_message_handler
   */
  void (*log_callback)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * @brief Initialize notifications
   * @param app_name application name for notifications
   */
  void init_notify(const char *app_name) {
    if (!notify_is_initted()) {
      std::scoped_lock lock(notification_mutex);
      notify_init(app_name);
    }
  }

  /**
   * @brief Acknowledge/click current notification
   * @param run_callback - Run notification callback when acknowledging
   */
  void acknowledge_notification(const bool run_callback = false) {
    if (notify_is_initted()) {
      std::scoped_lock lock(notification_mutex);
      if (notification_current != nullptr && NOTIFY_IS_NOTIFICATION(notification_current)) {
        if (run_callback && notification_current_callback != nullptr) {
          notification_current_callback();
        }
        notify_notification_close(notification_current, nullptr);
        g_object_unref(G_OBJECT(notification_current));
        notification_current = nullptr;
        notification_current_callback = nullptr;
      }
    } else if (qt_tray_menu != nullptr) {
      qt_tray_menu->clickMessage();
    }
  }

  /**
   * @brief Handle tray notifications via desktop-independent interface
   * @param tray Tray structure containing notification information
   */
  void notify(struct tray *tray) {
    if (tray->notification_text == nullptr || std::string(tray->notification_text).empty()) {
      return;
    }
    // Try to notify using libnotify
    if (notify_is_initted()) {
      if (notification_current != nullptr) {
        acknowledge_notification();
      }
      std::scoped_lock lock(notification_mutex);
      std::filesystem::path notification_icon = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
      if (std::filesystem::exists(notification_icon)) {
        // Use absolute path for filesystem icon files, not a relative one
        notification_icon = std::filesystem::absolute(notification_icon);
      }
      notification_current = notify_notification_new(tray->notification_title, tray->notification_text, notification_icon.c_str());
      if (notification_current != nullptr && NOTIFY_IS_NOTIFICATION(notification_current)) {
        if (tray->notification_cb != nullptr) {
          notification_current_callback = tray->notification_cb;
          notify_notification_add_action(notification_current, "default", "Default", NOTIFY_ACTION_CALLBACK(tray->notification_cb), nullptr, nullptr);
        }
        if (notify_notification_show(notification_current, nullptr)) {
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
   * @brief Uninitialize notifications
   */
  void uninit_notify() {
    if (notify_is_initted()) {
      acknowledge_notification();
      std::scoped_lock lock(notification_mutex);
      notify_uninit();
    }
  }

  /**
   * @brief Update notification app name
   * @param app_name the current application name
   */
  void set_notify_app_info(const char *app_name) {
    if (app_name) {
      uninit_notify();
      init_notify(app_name);
    }
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
    tray_linux::init_notify("tray");
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
    tray_linux::log_callback = cb;
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
    tray_linux::acknowledge_notification(true);
  }
}  // extern "C"
