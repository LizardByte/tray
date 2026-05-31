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
#include <thread>
#include <utility>

// lib includes
#include <libnotify/notify.h>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

namespace tray_linux {
  /**
   * Notification element struct
   */
  struct notification_data {
    /**
     * @brief Notification object
     */
    NotifyNotification *obj = nullptr;
    /**
     * @brief Notification callback
     */
    void (*cb)() = nullptr;
    /**
     * @brief Notification shown indicator
     */
    bool shown = false;
    /**
     * @brief Notification mutex for async thread synchronization
     */
    std::mutex mutex;
  };

  /**
   * Currently shown notifications
   */
  std::vector<std::shared_ptr<notification_data>> notifications;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Lock for currently shown notifications vector
   */
  std::mutex notifications_mutex;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * QtTrayMenu instance
   */
  std::unique_ptr<QtTrayMenu> qt_tray_menu = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  /**
   * Logging callback for qt_message_handler
   */
  void (*log_callback)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const

  /**
   * @brief Acknowledge notification asynchronously with timeout to avoid Dbus lockups
   * @param notification - Tray notification to close
   * @param timeout - optional timeout for async run in ms (default: 1000)
   */
  void async_tray_notification_acknowledge_(const std::shared_ptr<notification_data> &notification, int timeout = 1000) {
    std::thread t([notification]() {  // NOSONAR(cpp:S6168) - jthread is only available on C++20 onwards
      std::scoped_lock lock(notification->mutex);
      if (notification->shown && notification->obj != nullptr && NOTIFY_IS_NOTIFICATION(notification->obj) && notify_notification_close(notification->obj, nullptr)) {
        notification->shown = false;
        g_object_unref(G_OBJECT(notification->obj));
        notification->obj = nullptr;
        notification->cb = nullptr;
      }
    });
    while (notification->obj != nullptr && timeout > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      timeout -= 10;
    }
    if (timeout > 0) {
      // Finished. Join thread.
      t.join();
    } else {
      // Timed out. Detach thread and continue.
      t.detach();  // NOSONAR(cpp:S5962) - Keep this running until it times out by itself (usually after 25 seconds due to DBus)
    }
  }

  /**
   * @brief Acknowledge/click current notifications
   * @param run_callback - Run notification callback when acknowledging
   */
  void acknowledge_notifications(bool run_callback = false) {
    if (notify_is_initted()) {
      std::scoped_lock lock(notifications_mutex);
      for (auto notification : notifications) {
        if (run_callback && notification->cb != nullptr) {
          notification->cb();
        }
        async_tray_notification_acknowledge_(notification);
      }
      notifications.clear();
    } else if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      qt_tray_menu->clickMessage();
    }
  }

  /**
   * @brief Show notification asynchronously with timeout to avoid Dbus lockups
   * @param notification - Tray notification to show
   * @param timeout - optional timeout for async run in ms (default: 1000)
   */
  void async_tray_notification_show_(const std::shared_ptr<notification_data> &notification, int timeout = 1000) {
    std::thread t([notification]() {  // NOSONAR(cpp:S6168) - jthread is only available on C++20 onwards
      std::scoped_lock lock(notification->mutex);
      if (notification->obj != nullptr && NOTIFY_IS_NOTIFICATION(notification->obj) && notify_notification_show(notification->obj, nullptr)) {
        notification->shown = true;
      }
    });
    while (!notification->shown && timeout > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      timeout -= 10;
    }
    if (timeout > 0) {
      // Finished. Join thread.
      t.join();
    } else {
      // Timed out. Detach thread and continue.
      t.detach();  // NOSONAR(cpp:S5962) - Keep this running until it times out by itself (usually after 25 seconds due to DBus)
    }
  }

  /**
   * @brief Show tray notification via desktop-independent interface
   * @param tray Tray structure containing notification information
   */
  void notify(struct tray *tray) {
    if (tray->notification_text == nullptr || std::string(tray->notification_text).empty()) {
      return;
    }
    // Try to notify using libnotify
    if (notify_is_initted()) {
      if (!notifications.empty()) {
        acknowledge_notifications();
      }
      std::scoped_lock lock(notifications_mutex);
      std::filesystem::path notification_icon = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
      if (std::filesystem::exists(notification_icon)) {
        // Use absolute path for filesystem icon files, not a relative one
        notification_icon = std::filesystem::absolute(notification_icon);
      }
      auto notification = std::make_shared<struct notification_data>();
      notification->obj = notify_notification_new(tray->notification_title, tray->notification_text, notification_icon.c_str());
      if (notification->obj != nullptr && NOTIFY_IS_NOTIFICATION(notification->obj)) {
        if (tray->notification_cb != nullptr) {
          notification->cb = tray->notification_cb;
          notify_notification_add_action(notification->obj, "default", "Default", NOTIFY_ACTION_CALLBACK(tray->notification_cb), nullptr, nullptr);
        }
        notifications.emplace_back(notification);
        async_tray_notification_show_(notification);
      }
    } else if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      // Fallback to QtTrayMenu notification
      qt_tray_menu->showMessage(tray->notification_title, tray->notification_text, tray->notification_icon, tray->notification_cb);
    }
  }

  /**
   * @brief Initialize notifications
   * @param app_name application name for notifications
   * @return true if successful
   */
  bool init_notify(const char *app_name) {
    if (!notify_is_initted()) {
      if (!notifications.empty()) {
        acknowledge_notifications();
      }
      return notify_init(app_name);
    }
    return true;  // Already initialized, so init was successful
  }

  /**
   * @brief Uninitialize notifications
   */
  void uninit_notify() {
    if (notify_is_initted()) {
      acknowledge_notifications();
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
      // Check if a (wayland_)display is set or fallback to minimal QPA platform
      if (qgetenv("WAYLAND_DISPLAY").isEmpty() && qgetenv("DISPLAY").isEmpty()) {
        // Force fallback to QT platform minimal if no (WAYLAND_)DISPLAY was found
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("minimal"));
        qWarning("QtTrayMenu: no reachable WAYLAND_DISPLAY or DISPLAY endpoint, forcing QT_QPA_PLATFORM=minimal");
      }
      // Create a new unique pointer to QtTrayMenu instance
      tray_linux::qt_tray_menu = std::make_unique<QtTrayMenu>();
    }

    if (const auto result = tray_linux::qt_tray_menu->init(tray, false); result < 0) {
      // Tray init failed. Clean up and return error.
      tray_exit();
      return result;
    }

    if (!tray_linux::init_notify("tray") && !QtTrayMenu::supportsMessages()) {
      // Notification init failed. Clean up and return error.
      tray_exit();
      return -1;
    }

    // Fire notification if there is one
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
    tray_linux::acknowledge_notifications(true);
  }
}  // extern "C"
