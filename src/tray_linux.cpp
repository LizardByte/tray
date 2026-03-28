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

// Qt includes
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPixmap>
#include <QScreen>
#include <QStringList>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

/**
 * @brief Handles D-Bus notification action signals.
 *
 * Receives the org.freedesktop.Notifications ActionInvoked signal so that
 * notification click callbacks work when notifications are sent via D-Bus
 * rather than Qt's built-in balloon (QSystemTrayIcon::showMessage).
 *
 * Defined in tray_linux.cpp rather than a separate header to keep the moc
 * output self-contained via the inline `#include "tray_linux.moc"` at the
 * bottom of this file. Any CMake target that compiles tray_linux.cpp with
 * AUTOMOC ON will automatically generate and inline the moc output.
 */
class TrayNotificationHandler: public QObject {
  Q_OBJECT

public:
  uint notification_id = 0;  ///< ID of the most recently sent D-Bus notification.
  void (*cb)() = nullptr;  ///< Callback to invoke when the notification is activated.

public slots:

  /**
   * @brief Invoked when a D-Bus notification action is triggered.
   * @param id The notification ID.
   * @param action_key The action key that was triggered.
   */
  void onActionInvoked(uint id, const QString &action_key) const {
    if (id == notification_id && cb != nullptr && action_key == QLatin1String("default")) {
      cb();
    }
  }
};

namespace {
  std::unique_ptr<QApplication> g_app;  // NOSONAR(cpp:S5421) - mutable state, not const
  QSystemTrayIcon *g_tray_icon = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  TrayNotificationHandler *g_notification_handler = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  int g_loop_result = 0;  // NOSONAR(cpp:S5421) - mutable state, not const
  bool g_app_owned = false;  // NOSONAR(cpp:S5421) - mutable state, not const
  std::atomic<bool> g_exit_pending {false};  // NOSONAR(cpp:S5421) - written from any thread, read from tray_loop
  uint g_notification_id = 0;  // NOSONAR(cpp:S5421) - tracks last D-Bus notification ID for proper cleanup
  std::uint64_t g_notification_generation = 0;  // NOSONAR(cpp:S5421) - invalidates stale async Notify replies
  std::uint64_t g_notification_active_generation = 0;  // NOSONAR(cpp:S5421) - generation currently allowed to own notification_id
  void (*g_log_cb)(int, const char *) = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  QString g_app_name;  // NOSONAR(cpp:S5421) - set via tray_set_app_info before tray_init
  QString g_app_display_name;  // NOSONAR(cpp:S5421) - set via tray_set_app_info before tray_init
  QString g_desktop_name;  // NOSONAR(cpp:S5421) - set via tray_set_app_info before tray_init

  /**
   * @brief Invoke @p f on the Qt application's thread.
   *
   * When the caller is already on the Qt thread (or there is no QApplication),
   * @p f is called directly. When called from any other thread,
   * QMetaObject::invokeMethod with Qt::BlockingQueuedConnection is used so that
   * the caller blocks until the Qt thread finishes executing @p f. This ensures
   * all Qt GUI operations happen on the thread that owns the QApplication,
   * preventing cross-thread Qt object access that causes D-Bus relay warnings.
   *
   * Requires Qt 5.10+.
   *
   * @param f Callable to execute on the Qt thread.
   */
  template<typename Func>
  void run_on_qt_thread(Func f) {
    QCoreApplication *app = QCoreApplication::instance();
    if (app == nullptr || QThread::currentThread() == app->thread()) {
      f();
      return;
    }
    QMetaObject::invokeMethod(app, std::move(f), Qt::BlockingQueuedConnection);
  }

  bool is_wayland_session() {
    if (const QString platform = QGuiApplication::platformName().toLower();
        platform.contains(QStringLiteral("wayland"))) {
      return true;
    }
    return !qgetenv("WAYLAND_DISPLAY").isEmpty();
  }

  bool has_wayland_display_endpoint() {
    const QByteArray wayland_display = qgetenv("WAYLAND_DISPLAY");
    if (wayland_display.isEmpty()) {
      return false;
    }

    const QString display_name = QString::fromLocal8Bit(wayland_display).trimmed();
    if (display_name.isEmpty()) {
      return false;
    }

    if (const QFileInfo direct_path(display_name); direct_path.exists()) {
      return true;
    }

    const QByteArray runtime_dir = qgetenv("XDG_RUNTIME_DIR");
    if (runtime_dir.isEmpty()) {
      return false;
    }

    const QString socket_path = QDir(QString::fromLocal8Bit(runtime_dir)).filePath(display_name);
    return QFileInfo::exists(socket_path);
  }

  QString discover_wayland_display_name() {
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
      return QString();
    }

    const QByteArray runtime_dir_env = qgetenv("XDG_RUNTIME_DIR");
    if (runtime_dir_env.isEmpty()) {
      return QString();
    }

    const QString runtime_dir_path = QString::fromLocal8Bit(runtime_dir_env).trimmed();
    if (runtime_dir_path.isEmpty()) {
      return QString();
    }

    const QDir runtime_dir(runtime_dir_path);
    if (!runtime_dir.exists()) {
      return QString();
    }

    const QStringList entries = runtime_dir.entryList(
      QStringList() << QStringLiteral("wayland-*"),
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System,
      QDir::Name
    );
    if (entries.isEmpty()) {
      return QString();
    }

    QString selected;
    for (const QString &entry : entries) {
      if (const QString candidate_path = runtime_dir.filePath(entry); !QFileInfo::exists(candidate_path)) {
        continue;
      }
      if (entry == QStringLiteral("wayland-0")) {
        return entry;
      }
      if (selected.isEmpty()) {
        selected = entry;
      }
    }
    return selected;
  }

  bool try_autodiscover_wayland_display() {
    const QString discovered = discover_wayland_display_name();
    if (discovered.isEmpty()) {
      return false;
    }
    return qputenv("WAYLAND_DISPLAY", discovered.toLocal8Bit());
  }

  bool has_x11_display_endpoint() {
    const QByteArray display_env = qgetenv("DISPLAY");
    if (display_env.isEmpty()) {
      return false;
    }

    const QString display = QString::fromLocal8Bit(display_env).trimmed();
    if (display.isEmpty()) {
      return false;
    }

    if (display.startsWith('/')) {
      return QFileInfo::exists(display);
    }

    if (!display.startsWith(':')) {
      // Remote/TCP displays are not locally discoverable; treat as potentially usable.
      return true;
    }

    int digit_end = 1;
    while (digit_end < display.size() && display.at(digit_end).isDigit()) {
      digit_end++;
    }
    if (digit_end == 1) {
      return true;
    }

    bool ok = false;
    const int display_number = display.mid(1, digit_end - 1).toInt(&ok);
    if (!ok) {
      return true;
    }

    const QString socket_path = QStringLiteral("/tmp/.X11-unix/X%1").arg(display_number);
    return QFileInfo::exists(socket_path);
  }

  bool should_force_headless_qpa_fallback() {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
      return false;
    }
    return !has_wayland_display_endpoint() && !has_x11_display_endpoint();
  }

  QPoint screen_anchor_point(const QScreen *screen) {
    if (screen == nullptr) {
      return QPoint();
    }

    const QRect full = screen->geometry();
    const QRect avail = screen->availableGeometry();

    if (avail.top() > full.top()) {
      return QPoint(avail.right(), avail.top());
    }
    if (avail.bottom() < full.bottom()) {
      return QPoint(avail.right(), avail.bottom());
    }
    if (avail.left() > full.left()) {
      return QPoint(avail.left(), avail.bottom());
    }
    if (avail.right() < full.right()) {
      return QPoint(avail.right(), avail.bottom());
    }

    // Some compositors report no reserved panel area; top-right is a safer fallback than (0, 0).
    return avail.topRight();
  }

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
   * @brief Calculate the best position to show the context menu.
   *
   * Priority:
   * 1. Tray icon geometry (reliable on X11/XEmbed, sometimes on SNI).
   * 2. On a pure Xorg session, QCursor::pos() is accurate.
   * 3. On a Wayland session (detected via WAYLAND_DISPLAY), QCursor::pos() goes
   *    through XWayland and reflects the last X11 cursor position, which is NOT
   *    updated when the pointer interacts with Wayland-native surfaces such as the
   *    GNOME Shell top bar. A screen-geometry heuristic is used instead: the panel
   *    edge is inferred from the difference between the screen's full and available
   *    geometries.
   *
   * Qt's QMenu::popup() will adjust the final position to keep the menu fully
   * on-screen, including flipping it above the anchor point when needed.
   *
   * @return The point at which to show the context menu.
   */
  QPoint calculateMenuPosition(const QPoint &preferred_pos = QPoint()) {
    if (g_tray_icon != nullptr) {
      const QRect iconGeo = g_tray_icon->geometry();
      if (iconGeo.isValid()) {
        return iconGeo.bottomLeft();
      }
    }

    if (!preferred_pos.isNull() && !is_wayland_session()) {
      return preferred_pos;
    }

    // When running under a Wayland compositor, XWayland cursor coordinates are stale
    // for events originating from Wayland-native surfaces (e.g., the GNOME top bar).
    // Detect a Wayland session regardless of the Qt platform plugin in use.
    if (const bool wayland_session = is_wayland_session(); !wayland_session) {
      // Pure Xorg: QCursor::pos() is accurate.
      return QCursor::pos();
    }

    const QPoint cursor_pos = QCursor::pos();
    if (!cursor_pos.isNull()) {
      const QScreen *cursor_screen = QGuiApplication::screenAt(cursor_pos);
      if (cursor_screen != nullptr) {
        return cursor_pos;
      }
    }

    // Wayland session fallback: infer panel anchor from the relevant screen.
    const QScreen *screen = QGuiApplication::screenAt(cursor_pos);
    if (screen == nullptr) {
      screen = QGuiApplication::primaryScreen();
    }
    if (const QPoint anchored = screen_anchor_point(screen); !anchored.isNull()) {
      return anchored;
    }

    return cursor_pos;
  }

  QIcon icon_from_source(const QString &icon_source) {
    if (icon_source.isEmpty()) {
      return QIcon();
    }

    if (const QFileInfo icon_fi(icon_source); icon_fi.exists()) {
      const QString file_path = icon_fi.absoluteFilePath();
      if (const QIcon file_icon(file_path); !file_icon.isNull()) {
        return file_icon;
      }

      const QPixmap pixmap(file_path);
      if (!pixmap.isNull()) {
        QIcon icon;
        icon.addPixmap(pixmap);
        return icon;
      }
    }

    if (const QIcon themed = QIcon::fromTheme(icon_source); !themed.isNull()) {
      return themed;
    }

    return QIcon();
  }

  QIcon resolve_tray_icon(const struct tray *tray_data) {
    if (tray_data == nullptr) {
      return QIcon();
    }

    if (tray_data->icon != nullptr) {
      const QIcon icon = icon_from_source(QString::fromUtf8(tray_data->icon));
      if (!icon.isNull()) {
        return icon;
      }
    }

    if (tray_data->iconPathCount > 0 && tray_data->iconPathCount < 64) {
      for (int i = 0; i < tray_data->iconPathCount; i++) {
        if (tray_data->allIconPaths[i] == nullptr) {
          continue;
        }
        const QIcon icon = icon_from_source(QString::fromUtf8(tray_data->allIconPaths[i]));
        if (!icon.isNull()) {
          return icon;
        }
      }
    }

    return QIcon();
  }

  void popup_menu_for_activation(const QPoint &preferred_pos, int retries_left = 3) {
    if (g_tray_icon == nullptr) {
      return;
    }

    QMenu *menu = g_tray_icon->contextMenu();
    if (menu == nullptr || menu->isVisible()) {
      return;
    }

    menu->activateWindow();
    menu->setWindowFlag(Qt::Popup, true);
    menu->popup(calculateMenuPosition(preferred_pos));
    menu->setFocus(Qt::PopupFocusReason);

    if (!menu->isVisible() && retries_left > 0) {
      QTimer::singleShot(30, g_tray_icon, [preferred_pos, retries_left]() {
        popup_menu_for_activation(preferred_pos, retries_left - 1);
      });
    }
  }

  void close_notification_id(uint notification_id) {
    if (notification_id == 0) {
      return;
    }
    QDBusInterface iface(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications")
    );
    if (iface.isValid()) {
      iface.asyncCall(QStringLiteral("CloseNotification"), notification_id);
    }
  }

  void close_notification() {
    if (g_notification_id == 0) {
      return;
    }
    const uint id_to_close = g_notification_id;
    g_notification_id = 0;
    close_notification_id(id_to_close);
  }

  QMenu *build_menu(struct tray_menu *m, QWidget *parent) {
    auto *menu = new QMenu(parent);  // NOSONAR(cpp:S5025) - submenus owned by parent via Qt; top-level deleted manually
    for (; m != nullptr && m->text != nullptr; m++) {
      if (std::strcmp(m->text, "-") == 0) {
        menu->addSeparator();
      } else if (m->submenu != nullptr) {
        QMenu *sub = build_menu(m->submenu, menu);
        sub->setTitle(QString::fromUtf8(m->text));
        QAction *sub_action = menu->addMenu(sub);
        sub_action->setEnabled(m->disabled == 0);
      } else {
        auto *action = menu->addAction(QString::fromUtf8(m->text));
        action->setEnabled(m->disabled == 0);
        if (m->checkbox) {
          action->setCheckable(true);
          action->setChecked(m->checked != 0);
        }
        action->setData(QVariant::fromValue(static_cast<void *>(m)));
        QObject::connect(action, &QAction::triggered, menu, [action]() {
          auto *item = static_cast<struct tray_menu *>(action->data().value<void *>());
          if (item != nullptr && item->cb != nullptr) {
            item->cb(item);
          }
        });
      }
    }
    return menu;
  }

  bool menu_layout_matches(const QMenu *menu, const struct tray_menu *items) {
    if (menu == nullptr) {
      return false;
    }

    const QList<QAction *> actions = menu->actions();
    int action_index = 0;
    for (const struct tray_menu *item = items; item != nullptr && item->text != nullptr; item++) {
      if (action_index >= actions.size()) {
        return false;
      }

      const int current_action_index = action_index;
      action_index++;
      const QAction *action = actions.at(current_action_index);
      if (std::strcmp(item->text, "-") == 0) {
        if (!action->isSeparator()) {
          return false;
        }
        continue;
      }

      if (item->submenu != nullptr) {
        const QMenu *submenu = action->menu();
        if (submenu == nullptr || !menu_layout_matches(submenu, item->submenu)) {
          return false;
        }
      } else if (action->isSeparator() || action->menu() != nullptr) {
        return false;
      }
    }

    return action_index == actions.size();
  }

  void update_menu_state(const QMenu *menu, struct tray_menu *items) {
    if (menu == nullptr || items == nullptr) {
      return;
    }

    const QList<QAction *> actions = menu->actions();
    int action_index = 0;
    for (struct tray_menu *item = items; item != nullptr && item->text != nullptr; item++) {
      const int current_action_index = action_index;
      action_index++;
      QAction *action = actions.at(current_action_index);
      if (std::strcmp(item->text, "-") == 0) {
        continue;
      }

      action->setText(QString::fromUtf8(item->text));
      action->setEnabled(item->disabled == 0);
      if (item->submenu != nullptr) {
        update_menu_state(action->menu(), item->submenu);
        continue;
      }

      action->setCheckable(item->checkbox != 0);
      if (item->checkbox != 0) {
        action->setChecked(item->checked != 0);
      }
      action->setData(QVariant::fromValue(static_cast<void *>(item)));
    }
  }

  void configure_app_metadata(const struct tray *tray) {
    const QString effective_name = !g_app_name.isEmpty() ? g_app_name : QStringLiteral("tray");
    if (QCoreApplication::applicationName().isEmpty()) {
      QCoreApplication::setApplicationName(effective_name);
    }

    if (QGuiApplication::applicationDisplayName().isEmpty()) {
      if (!g_app_display_name.isEmpty()) {
        QGuiApplication::setApplicationDisplayName(g_app_display_name);
      } else {
        const QString display_name =
          (tray != nullptr && tray->tooltip != nullptr) ? QString::fromUtf8(tray->tooltip) : effective_name;
        QGuiApplication::setApplicationDisplayName(display_name);
      }
    }

    if (!QGuiApplication::desktopFileName().isEmpty()) {
      return;
    }

    if (!g_desktop_name.isEmpty()) {
      QGuiApplication::setDesktopFileName(g_desktop_name);
      return;
    }

    QString desktop_name = QCoreApplication::applicationName();
    if (!desktop_name.endsWith(QStringLiteral(".desktop"))) {
      desktop_name += QStringLiteral(".desktop");
    }
    QGuiApplication::setDesktopFileName(desktop_name);
  }

  void connect_activation_handler() {
    // Show the context menu on left-click (Trigger).
    // Qt handles right-click natively via setContextMenu on both X11/XEmbed and
    // SNI (Wayland/AppIndicators), so we do not handle Context here.
    // The menu position is captured immediately before deferring by a short timer.
    // Deferring allows any platform pointer grab from the tray click to be released
    // before the menu establishes its own grab.
    QObject::connect(g_tray_icon, &QSystemTrayIcon::activated, [](QSystemTrayIcon::ActivationReason reason) {
      if (const bool left_click_activation =
            (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::Context);
          !left_click_activation) {
        return;
      }

      const QPoint click_pos = QCursor::pos();
      QTimer::singleShot(30, g_tray_icon, [click_pos]() {
        popup_menu_for_activation(click_pos);
      });
    });
  }

  void ensure_notification_handler_connected() {
    if (g_notification_handler != nullptr) {
      return;
    }

    g_notification_handler = new TrayNotificationHandler();  // NOSONAR(cpp:S5025) - deleted in destroy_app()
    // Defer D-Bus ActionInvoked handler setup to the first event-loop iteration.
    // Creating QDBusConnection socket notifiers before the event loop starts can
    // trigger a "QSocketNotifier: Can only be used with threads started with QThread"
    // warning when the tray runs in a std::thread.
    QTimer::singleShot(0, g_notification_handler, []() {
      if (g_notification_handler == nullptr) {
        return;
      }
      QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("ActionInvoked"),
        g_notification_handler,
        SLOT(onActionInvoked(uint, QString))
      );
    });
  }

  void update_context_menu(const struct tray *tray) {
    if (tray->menu == nullptr) {
      return;
    }

    QMenu *existing_menu = g_tray_icon->contextMenu();
    if (existing_menu != nullptr && menu_layout_matches(existing_menu, tray->menu)) {
      update_menu_state(existing_menu, tray->menu);
      return;
    }

    // setContextMenu does not take ownership; delete the old menu before replacing it.
    QMenu *new_menu = build_menu(tray->menu, nullptr);  // NOSONAR(cpp:S5025) - deleted via existing_menu path or on next update
    g_tray_icon->setContextMenu(new_menu);
    if (existing_menu == nullptr) {
      return;
    }

    // hide() before delete releases any X11 pointer grab held by the popup.
    // Skipping this leaves the grab active, causing future popup menus to appear
    // but receive no pointer events, so QAction::triggered is never emitted.
    existing_menu->hide();
    delete existing_menu;  // NOSONAR(cpp:S5025) - required; Qt does not own this
  }

  void reset_notification_state() {
    g_notification_generation++;
    g_notification_active_generation = 0;
    if (g_notification_handler != nullptr) {
      g_notification_handler->notification_id = 0;
      g_notification_handler->cb = nullptr;
    }
    if (g_tray_icon != nullptr) {
      QObject::disconnect(g_tray_icon, &QSystemTrayIcon::messageClicked, nullptr, nullptr);
    }
    close_notification();
  }

  QString resolve_notification_icon(const struct tray *tray) {
    const char *icon_path = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
    if (icon_path == nullptr) {
      return QString();
    }

    if (const QFileInfo fi(QString::fromUtf8(icon_path)); fi.exists()) {
      return QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
    }
    return QString::fromUtf8(icon_path);
  }

  void destroy_tray();

  void handle_notification_reply(QDBusPendingCallWatcher *watcher, const std::uint64_t notification_generation) {
    const QDBusPendingReply<uint> reply = *watcher;
    if (!reply.isValid() || g_tray_icon == nullptr) {
      watcher->deleteLater();
      return;
    }

    const uint reply_id = reply.value();
    const bool stale_reply =
      notification_generation != g_notification_active_generation || g_notification_active_generation == 0;
    if (stale_reply) {
      // The request was cleared or superseded before Notify returned; close it immediately.
      close_notification_id(reply_id);
      watcher->deleteLater();
      return;
    }

    g_notification_id = reply_id;
    if (g_notification_handler != nullptr) {
      g_notification_handler->notification_id = g_notification_id;
    }
    watcher->deleteLater();
  }

  bool send_dbus_notification(
    const struct tray *tray,
    const QString &title,
    const QString &text,
    const QString &icon,
    const std::uint64_t notification_generation
  ) {
    QVariantMap hints;
    if (!icon.isEmpty()) {
      hints[QStringLiteral("image-path")] = icon;
    }

    QDBusInterface iface(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications")
    );
    if (!iface.isValid()) {
      return false;
    }

    QStringList actions;
    if (tray->notification_cb != nullptr) {
      actions << QStringLiteral("default") << QString();
    }
    if (g_notification_handler != nullptr) {
      g_notification_handler->cb = tray->notification_cb;
    }

    QDBusPendingCall pending = iface.asyncCall(
      QStringLiteral("Notify"),
      QGuiApplication::applicationDisplayName(),
      static_cast<uint>(0),
      icon,
      title,
      text,
      actions,
      hints,
      5000
    );
    auto *watcher = new QDBusPendingCallWatcher(pending);  // NOSONAR(cpp:S5025) - deleted via deleteLater in finished handler
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [notification_generation](QDBusPendingCallWatcher *finished) {
      handle_notification_reply(finished, notification_generation);
    });
    return true;
  }

  void send_qt_notification_fallback(const struct tray *tray, const QString &title, const QString &text) {
    if (tray->notification_cb != nullptr && g_notification_handler != nullptr) {
      g_notification_handler->cb = tray->notification_cb;
      QObject::connect(g_tray_icon, &QSystemTrayIcon::messageClicked, []() {
        if (g_notification_handler == nullptr || g_notification_handler->cb == nullptr) {
          return;
        }
        g_notification_handler->cb();
      });
    }
    g_tray_icon->showMessage(title, text, QSystemTrayIcon::Information, 5000);
  }

  void update_notification(const struct tray *tray) {
    const QString text = tray->notification_text != nullptr ? QString::fromUtf8(tray->notification_text) : QString();
    reset_notification_state();
    if (text.isEmpty()) {
      return;
    }

    const std::uint64_t notification_generation = g_notification_generation;
    g_notification_active_generation = notification_generation;

    const QString title = tray->notification_title != nullptr ? QString::fromUtf8(tray->notification_title) : QString();
    const QString icon = resolve_notification_icon(tray);

    if (!send_dbus_notification(tray, title, text, icon, notification_generation)) {
      // D-Bus may be unavailable; fall back to Qt's built-in balloon.
      send_qt_notification_fallback(tray, title, text);
    }
  }

  void update_tray_state(const struct tray *tray) {
    if (g_tray_icon == nullptr) {
      return;
    }

    QIcon tray_icon = resolve_tray_icon(tray);
    if (tray_icon.isNull() && !g_tray_icon->icon().isNull()) {
      tray_icon = g_tray_icon->icon();
    }
    if (tray_icon.isNull()) {
      tray_icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    if (!tray_icon.isNull()) {
      g_tray_icon->setIcon(tray_icon);
    }

    if (tray->tooltip != nullptr) {
      g_tray_icon->setToolTip(QString::fromUtf8(tray->tooltip));
    }

    update_context_menu(tray);
    update_notification(tray);
  }

  void initialize_tray(struct tray *tray, int *result) {
    destroy_tray();
    g_loop_result = 0;
    g_exit_pending = false;

    g_tray_icon = new QSystemTrayIcon();  // NOSONAR(cpp:S5025) - raw pointer; deleted in destroy_tray() before QApplication
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      destroy_tray();
      *result = -1;
      return;
    }

    configure_app_metadata(tray);
    connect_activation_handler();
    ensure_notification_handler_connected();
    update_tray_state(tray);
    g_tray_icon->show();
  }

  void destroy_tray() {
    reset_notification_state();
    if (g_tray_icon != nullptr) {
      g_tray_icon->hide();
      QMenu *menu = g_tray_icon->contextMenu();
      g_tray_icon->setContextMenu(nullptr);
      delete g_tray_icon;  // NOSONAR(cpp:S5025) - raw pointer; deleted explicitly before QApplication is destroyed
      g_tray_icon = nullptr;
      if (menu != nullptr) {
        menu->hide();
        delete menu;  // NOSONAR(cpp:S5025) - QSystemTrayIcon does not own the context menu
      }
    }
  }

  void destroy_app() {
    if (g_notification_handler != nullptr) {
      delete g_notification_handler;  // NOSONAR(cpp:S5025) - raw pointer; deleted explicitly before QApplication
      g_notification_handler = nullptr;
    }
    if (g_app_owned && g_app) {
      // Destroy QApplication here (during active program execution) rather than letting
      // the unique_ptr destructor run at static-destruction time. At static-destruction
      // time, Qt's lazily-initialized D-Bus statics have already been destroyed (LIFO
      // order), so calling QApplication::~QApplication() then would crash.
      g_app.reset();
      g_app_owned = false;
    }
  }
}  // namespace

extern "C" {

  int tray_init(struct tray *tray) {
    if (QApplication::instance() == nullptr) {
      if (try_autodiscover_wayland_display() && g_log_cb != nullptr) {
        g_log_cb(1, "Qt tray: auto-discovered WAYLAND_DISPLAY from XDG_RUNTIME_DIR");
      }
      if (should_force_headless_qpa_fallback()) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("minimal"));
        if (g_log_cb != nullptr) {
          g_log_cb(2, "Qt tray: no reachable WAYLAND_DISPLAY or DISPLAY endpoint, forcing QT_QPA_PLATFORM=minimal");
        }
      }
      static int argc = 0;
      g_app = std::make_unique<QApplication>(argc, nullptr);
      g_app_owned = true;
    }

    int result = 0;
    run_on_qt_thread([tray, &result]() {
      initialize_tray(tray, &result);
    });
    return result;
  }

  int tray_loop(int blocking) {
    if (g_exit_pending) {
      g_exit_pending = false;
      run_on_qt_thread([]() {
        destroy_tray();
        destroy_app();
      });
      return g_loop_result;
    }

    if (blocking) {
      if (g_app_owned) {
        QApplication::exec();
        if (g_exit_pending) {
          g_exit_pending = false;
          destroy_tray();
          destroy_app();
        }
      } else {
        // An external event loop owns Qt processing; block until tray_exit() fires.
        while (!g_exit_pending) {
          QThread::msleep(10);
        }
        g_exit_pending = false;
        run_on_qt_thread([]() {
          destroy_tray();
          destroy_app();
        });
      }
    } else {
      if (g_app_owned) {
        QApplication::processEvents();
      } else {
        const QCoreApplication *app_inst = QCoreApplication::instance();
        if (app_inst != nullptr && QThread::currentThread() == app_inst->thread()) {
          QApplication::processEvents();
        }
        // On a non-Qt thread with an external app the external event loop handles processing.
      }
    }
    return g_loop_result;
  }

  void tray_update(struct tray *tray) {  // NOSONAR(cpp:S995) - C API requires this exact mutable-pointer signature
    run_on_qt_thread([tray]() {
      update_tray_state(tray);
    });
  }

  void tray_show_menu(void) {
    run_on_qt_thread([]() {
      if (g_tray_icon != nullptr) {
        const QMenu *menu = g_tray_icon->contextMenu();
        if (menu != nullptr) {
          popup_menu_for_activation(QPoint());
          QApplication::processEvents();
        }
      }
    });
  }

  void tray_simulate_notification_click(void) {
    run_on_qt_thread([]() {
      if (g_notification_handler != nullptr && g_notification_handler->cb != nullptr) {
        if (g_notification_handler->notification_id != 0) {
          // Simulate the D-Bus ActionInvoked signal for the current notification.
          g_notification_handler->onActionInvoked(
            g_notification_handler->notification_id,
            QStringLiteral("default")
          );
        } else {
          // Fallback path (no D-Bus): invoke the callback directly.
          g_notification_handler->cb();
        }
      }
    });
  }

  void tray_simulate_menu_item_click(int index) {
    run_on_qt_thread([index]() {
      if (g_tray_icon == nullptr || index < 0) {
        return;
      }
      const QMenu *menu = g_tray_icon->contextMenu();
      if (menu == nullptr) {
        return;
      }
      const QList<QAction *> actions = menu->actions();
      if (index >= actions.size()) {
        return;
      }
      QAction *action = actions.at(index);
      if (action == nullptr || action->isSeparator() || action->menu() != nullptr || !action->isEnabled()) {
        return;
      }
      action->trigger();
    });
  }

  void tray_exit(void) {
    g_loop_result = -1;
    g_exit_pending = true;
    if (g_app_owned) {
      run_on_qt_thread([]() {
        if (QApplication::instance() != nullptr) {
          QApplication::quit();
        }
      });
    }
  }

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {  // NOSONAR(cpp:S5205) - C API requires a plain function pointer callback type
    g_log_cb = cb;
    if (cb != nullptr) {
      qInstallMessageHandler(qt_message_handler);
    } else {
      qInstallMessageHandler(nullptr);
    }
  }

  void tray_set_app_info(const char *app_name, const char *app_display_name, const char *desktop_name) {
    g_app_name = app_name != nullptr ? QString::fromUtf8(app_name) : QString();
    g_app_display_name = app_display_name != nullptr ? QString::fromUtf8(app_display_name) : QString();
    g_desktop_name = desktop_name != nullptr ? QString::fromUtf8(desktop_name) : QString();
  }

}  // extern "C"

// Must be included at the end of a .cpp file when Q_OBJECT classes are defined
// in that .cpp (not in a header). AUTOMOC sees this directive and generates
// tray_linux.moc, which is then inlined here at compile time.
#include "tray_linux.moc"
