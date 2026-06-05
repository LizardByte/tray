/**
 * @file src/tray_qt.cpp
 * @brief System tray implementation using Qt.
 */
// standard includes
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(TRAY_USE_LIBNOTIFY)
// lib includes
  #include <libnotify/notify.h>
#endif

// qt includes
#include <QByteArray>
#include <QDebug>
#include <QMessageLogContext>
#include <QString>

// local includes
#include "QtTrayMenu.h"
#include "tray.h"

namespace tray_qt {
#if defined(TRAY_USE_LIBNOTIFY)
  /**
   * Notification element struct
   */
  struct notification_data {
    /**
     * Notification object
     */
    NotifyNotification *obj = nullptr;
    /**
     * Notification callback
     */
    void (*cb)() = nullptr;
    /**
     * Notification shown indicator
     */
    bool shown = false;
    /**
     * Notification mutex for async thread synchronization
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
#endif
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
   * @brief Get the effective application name used for notification backends.
   * @return application name as UTF-8 data
   */
  QByteArray effective_notify_app_name() {
    const QString effective_name = !app_name.isEmpty() ? app_name : QStringLiteral("tray");
    return effective_name.toUtf8();
  }

#if defined(TRAY_USE_LIBNOTIFY)
  /**
   * @brief Acknowledge notification asynchronously with timeout to avoid D-Bus lockups
   * @param notification Tray notification to close
   * @param timeout optional timeout for async run in ms
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
      t.join();
    } else {
      t.detach();  // NOSONAR(cpp:S5962) - Keep this running until it times out by itself, usually after 25 seconds due to D-Bus
    }
  }

  /**
   * @brief Show notification asynchronously with timeout to avoid D-Bus lockups
   * @param notification Tray notification to show
   * @param timeout optional timeout for async run in ms
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
      t.join();
    } else {
      t.detach();  // NOSONAR(cpp:S5962) - Keep this running until it times out by itself, usually after 25 seconds due to D-Bus
    }
  }

  void acknowledge_notification(bool run_callback);

  /**
   * @brief Initialize notifications
   * @param app_name application name for notifications
   * @return true if successful
   */
  bool init_notify(const char *app_name) {
    if (!notify_is_initted()) {
      if (!notifications.empty()) {
        acknowledge_notification();
      }
      return notify_init(app_name);
    }
    return true;  // Already initialized, so init was successful
  }
#else
  /**
   * @brief Initialize notifications
   * @return false because no native notification backend is compiled in
   */
  bool init_notify(const char *) {
    return false;
  }
#endif

  /**
   * @brief Acknowledge/click current notification
   * @param run_callback Run notification callback when acknowledging
   */
  void acknowledge_notification(const bool run_callback = false) {
#if defined(TRAY_USE_LIBNOTIFY)
    if (notify_is_initted()) {
      std::scoped_lock lock(notifications_mutex);
      for (const auto &notification : notifications) {
        if (run_callback && notification->cb != nullptr) {
          notification->cb();
        }
        async_tray_notification_acknowledge_(notification);
      }
      notifications.clear();
      return;
    }
#endif

    if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      qt_tray_menu->clickMessage();
    }
  }

  /**
   * @brief Clear current notification state without invoking callbacks.
   */
  void clear_notification() {
#if defined(TRAY_USE_LIBNOTIFY)
    if (notify_is_initted()) {
      acknowledge_notification();
    }
#endif

    if (qt_tray_menu != nullptr) {
      qt_tray_menu->clearMessageCallback();
    }
  }

  /**
   * @brief Show tray notification via desktop-independent interface
   * @param tray Tray structure containing notification information
   */
  void notify(struct tray *tray) {
    if (tray->notification_text == nullptr || std::string(tray->notification_text).empty()) {
      clear_notification();
      return;
    }
#if defined(TRAY_USE_LIBNOTIFY)
    // Try to notify using libnotify
    if (notify_is_initted()) {
      if (!notifications.empty()) {
        acknowledge_notification();
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
        return;
      }
    }
#endif
    // Fallback to QtTrayMenu notification
    if (qt_tray_menu != nullptr && QtTrayMenu::supportsMessages()) {
      qt_tray_menu->showMessage(tray->notification_title, tray->notification_text, tray->notification_icon, tray->notification_cb);
    }
  }

  /**
   * @brief Uninitialize notifications
   */
  void uninit_notify() {
#if defined(TRAY_USE_LIBNOTIFY)
    if (notify_is_initted()) {
      acknowledge_notification();
      notify_uninit();
    }
#endif
  }

  /**
   * @brief Update notification app name.
   */
  void set_notify_app_info() {
#if defined(TRAY_USE_LIBNOTIFY)
    if (!notify_is_initted()) {
      return;
    }

    uninit_notify();
    const QByteArray notify_app_name = effective_notify_app_name();
    init_notify(notify_app_name.constData());
#endif
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

    tray_qt::set_notify_app_info();
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

    const QByteArray notify_app_name = tray_qt::effective_notify_app_name();
    if (!tray_qt::init_notify(notify_app_name.constData()) && !QtTrayMenu::supportsMessages()) {
      // Notification init failed. Clean up and return error.
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
    tray_qt::uninit_notify();

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
    tray_qt::acknowledge_notification(true);
  }

}  // extern "C"
