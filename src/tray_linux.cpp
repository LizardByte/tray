/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <atomic>
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
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPixmap>
#include <QScreen>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>
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
  void onActionInvoked(uint id, const QString &action_key) {
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
    const QString platform = QGuiApplication::platformName().toLower();
    if (platform.contains(QStringLiteral("wayland"))) {
      return true;
    }
    return !qgetenv("WAYLAND_DISPLAY").isEmpty();
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
    const bool wayland_session = is_wayland_session();
    if (!wayland_session) {
      // Pure Xorg: QCursor::pos() is accurate.
      return QCursor::pos();
    }

    const QPoint cursor_pos = QCursor::pos();
    if (!cursor_pos.isNull()) {
      QScreen *cursor_screen = QGuiApplication::screenAt(cursor_pos);
      if (cursor_screen != nullptr) {
        return cursor_pos;
      }
    }

    // Wayland session fallback: infer panel anchor from the relevant screen.
    QScreen *screen = QGuiApplication::screenAt(cursor_pos);
    if (screen == nullptr) {
      screen = QGuiApplication::primaryScreen();
    }
    const QPoint anchored = screen_anchor_point(screen);
    if (!anchored.isNull()) {
      return anchored;
    }

    return cursor_pos;
  }

  QIcon icon_from_source(const QString &icon_source) {
    if (icon_source.isEmpty()) {
      return QIcon();
    }

    const QFileInfo icon_fi(icon_source);
    if (icon_fi.exists()) {
      const QString file_path = icon_fi.absoluteFilePath();
      const QIcon file_icon(file_path);
      if (!file_icon.isNull()) {
        return file_icon;
      }

      const QPixmap pixmap(file_path);
      if (!pixmap.isNull()) {
        QIcon icon;
        icon.addPixmap(pixmap);
        return icon;
      }
    }

    const QIcon themed = QIcon::fromTheme(icon_source);
    if (!themed.isNull()) {
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

  void close_notification() {
    if (g_notification_id == 0) {
      return;
    }
    const uint id_to_close = g_notification_id;
    g_notification_id = 0;
    QDBusInterface iface(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications")
    );
    if (iface.isValid()) {
      iface.asyncCall(QStringLiteral("CloseNotification"), id_to_close);
    }
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
        action->setData(QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(m)));
        QObject::connect(action, &QAction::triggered, menu, [action]() {
          auto *item = reinterpret_cast<struct tray_menu *>(action->data().value<quintptr>());
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

      QAction *action = actions.at(action_index++);
      if (std::strcmp(item->text, "-") == 0) {
        if (!action->isSeparator()) {
          return false;
        }
        continue;
      }

      if (item->submenu != nullptr) {
        QMenu *submenu = action->menu();
        if (submenu == nullptr || !menu_layout_matches(submenu, item->submenu)) {
          return false;
        }
      } else if (action->isSeparator() || action->menu() != nullptr) {
        return false;
      }
    }

    return action_index == actions.size();
  }

  void update_menu_state(QMenu *menu, struct tray_menu *items) {
    if (menu == nullptr || items == nullptr) {
      return;
    }

    const QList<QAction *> actions = menu->actions();
    int action_index = 0;
    for (struct tray_menu *item = items; item != nullptr && item->text != nullptr; item++) {
      QAction *action = actions.at(action_index++);
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
      action->setData(QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(item)));
    }
  }

  void destroy_tray() {
    close_notification();
    if (g_notification_handler != nullptr) {
      g_notification_handler->notification_id = 0;
      g_notification_handler->cb = nullptr;
    }
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
      QDBusConnection::sessionBus().disconnect(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("ActionInvoked"),
        g_notification_handler,
        SLOT(onActionInvoked(uint, QString))
      );
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
      static int argc = 0;
      g_app = std::make_unique<QApplication>(argc, nullptr);
      g_app_owned = true;
    }

    int result = 0;
    run_on_qt_thread([tray, &result]() {
      destroy_tray();
      g_loop_result = 0;
      g_exit_pending = false;

      g_tray_icon = new QSystemTrayIcon();  // NOSONAR(cpp:S5025) - raw pointer; deleted in destroy_tray() before QApplication

      if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        destroy_tray();
        result = -1;
        return;
      }

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
      if (QGuiApplication::desktopFileName().isEmpty()) {
        if (!g_desktop_name.isEmpty()) {
          QGuiApplication::setDesktopFileName(g_desktop_name);
        } else {
          QString desktop_name = QCoreApplication::applicationName();
          if (!desktop_name.endsWith(QStringLiteral(".desktop"))) {
            desktop_name += QStringLiteral(".desktop");
          }
          QGuiApplication::setDesktopFileName(desktop_name);
        }
      }

      // Show the context menu on left-click (Trigger).
      // Qt handles right-click natively via setContextMenu on both X11/XEmbed and
      // SNI (Wayland/AppIndicators), so we do not handle Context here.
      // The menu position is captured immediately before deferring by a short timer.
      // Deferring allows any
      // platform pointer grab from the tray click to be released before the menu
      // establishes its own grab.
      // activateWindow() gives the menu window X11 focus so that the subsequent
      // XGrabPointer inside popup() succeeds, enabling click-outside dismissal on Xorg.
      QObject::connect(g_tray_icon, &QSystemTrayIcon::activated, [](QSystemTrayIcon::ActivationReason reason) {
        const bool left_click_activation =
          (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::Context);

        if (!left_click_activation) {
          return;
        }

        const QPoint click_pos = QCursor::pos();
        QTimer::singleShot(30, g_tray_icon, [click_pos]() {
          popup_menu_for_activation(click_pos);
        });
      });

      // Defer D-Bus ActionInvoked handler setup to the first event-loop iteration.
      // Creating QDBusConnection socket notifiers before the event loop starts can
      // trigger a "QSocketNotifier: Can only be used with threads started with QThread"
      // warning when the tray runs in a std::thread. Deferring via QTimer::singleShot
      // ensures the socket notifiers are created while the event dispatcher is active.
      if (g_notification_handler == nullptr) {
        g_notification_handler = new TrayNotificationHandler();  // NOSONAR(cpp:S5025) - deleted in destroy_app()
        QTimer::singleShot(0, g_notification_handler, []() {
          if (g_notification_handler != nullptr) {
            QDBusConnection::sessionBus().connect(
              QStringLiteral("org.freedesktop.Notifications"),
              QStringLiteral("/org/freedesktop/Notifications"),
              QStringLiteral("org.freedesktop.Notifications"),
              QStringLiteral("ActionInvoked"),
              g_notification_handler,
              SLOT(onActionInvoked(uint, QString))
            );
          }
        });
      }

      tray_update(tray);
      g_tray_icon->show();
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
        QCoreApplication *app_inst = QCoreApplication::instance();
        if (app_inst != nullptr && QThread::currentThread() == app_inst->thread()) {
          QApplication::processEvents();
        }
        // On a non-Qt thread with an external app the external event loop handles processing.
      }
    }
    return g_loop_result;
  }

  void tray_update(struct tray *tray) {
    run_on_qt_thread([tray]() {
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
      // Only update the icon when the resolved icon is valid. Setting a null icon
      // clears the tray icon and triggers "No Icon set" warnings (Qt6 is stricter
      // about QIcon::fromTheme when the name is not found in the active theme).
      if (!tray_icon.isNull()) {
        g_tray_icon->setIcon(tray_icon);
      }

      if (tray->tooltip != nullptr) {
        g_tray_icon->setToolTip(QString::fromUtf8(tray->tooltip));
      }

      if (tray->menu != nullptr) {
        QMenu *existing_menu = g_tray_icon->contextMenu();
        if (existing_menu != nullptr && menu_layout_matches(existing_menu, tray->menu)) {
          update_menu_state(existing_menu, tray->menu);
        } else {
          // setContextMenu does not take ownership; delete the old menu before replacing it.
          QMenu *new_menu = build_menu(tray->menu, nullptr);  // NOSONAR(cpp:S5025) - deleted via old_menu path or on next update
          g_tray_icon->setContextMenu(new_menu);
          if (existing_menu != nullptr) {
            // hide() before delete releases any X11 pointer grab held by the popup.
            // Skipping this leaves the grab active, causing future popup menus to appear
            // but receive no pointer events, so QAction::triggered is never emitted.
            existing_menu->hide();
            delete existing_menu;  // NOSONAR(cpp:S5025) - required; Qt does not own this
          }
        }
      }

      const QString text = tray->notification_text != nullptr ? QString::fromUtf8(tray->notification_text) : QString();

      // Reset previous notification state before setting up the new one.
      if (g_notification_handler != nullptr) {
        g_notification_handler->notification_id = 0;
        g_notification_handler->cb = nullptr;
      }
      QObject::disconnect(g_tray_icon, &QSystemTrayIcon::messageClicked, nullptr, nullptr);
      close_notification();

      if (!text.isEmpty()) {
        const QString title = tray->notification_title != nullptr ? QString::fromUtf8(tray->notification_title) : QString();
        const char *icon_path = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
        QString icon;
        if (icon_path != nullptr) {
          QFileInfo fi(QString::fromUtf8(icon_path));
          icon = fi.exists() ? QUrl::fromLocalFile(fi.absoluteFilePath()).toString() : QString::fromUtf8(icon_path);
        }
        QVariantMap hints;
        if (!icon.isEmpty()) {
          hints[QStringLiteral("image-path")] = icon;
        }

        QDBusInterface iface(
          QStringLiteral("org.freedesktop.Notifications"),
          QStringLiteral("/org/freedesktop/Notifications"),
          QStringLiteral("org.freedesktop.Notifications")
        );
        if (iface.isValid()) {
          // Include the "default" action so that clicking the notification body fires ActionInvoked.
          // QSystemTrayIcon::messageClicked is NOT emitted for D-Bus-dispatched notifications,
          // so the callback must be routed through TrayNotificationHandler::onActionInvoked instead.
          QStringList actions;
          if (tray->notification_cb != nullptr) {
            actions << QStringLiteral("default") << QString();
          }
          // Store the callback before the async Notify so tray_simulate_notification_click works
          // even when the notification daemon is unavailable and the D-Bus reply is invalid.
          if (g_notification_handler != nullptr) {
            g_notification_handler->cb = tray->notification_cb;
          }
          // Use asyncCall to avoid entering a nested Qt event loop while waiting for the D-Bus
          // reply. A synchronous call here would allow re-entrant tray_update dispatches from
          // other threads (via BlockingQueuedConnection), which can delete and replace the context
          // QMenu mid-build, breaking all QAction signal connections.
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
          QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher]() {
            const QDBusPendingReply<uint> reply = *watcher;
            if (reply.isValid() && g_tray_icon != nullptr) {
              g_notification_id = reply.value();
              if (g_notification_handler != nullptr) {
                g_notification_handler->notification_id = g_notification_id;
              }
            }
            watcher->deleteLater();
          });
        } else {
          // D-Bus unavailable: fall back to Qt's built-in balloon and messageClicked signal.
          if (tray->notification_cb != nullptr && g_notification_handler != nullptr) {
            g_notification_handler->cb = tray->notification_cb;
            QObject::connect(g_tray_icon, &QSystemTrayIcon::messageClicked, []() {
              if (g_notification_handler != nullptr && g_notification_handler->cb != nullptr) {
                g_notification_handler->cb();
              }
            });
          }
          g_tray_icon->showMessage(title, text, QSystemTrayIcon::Information, 5000);
        }
      }
    });
  }

  void tray_show_menu(void) {
    run_on_qt_thread([]() {
      if (g_tray_icon != nullptr) {
        QMenu *menu = g_tray_icon->contextMenu();
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
      QMenu *menu = g_tray_icon->contextMenu();
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

  void tray_set_log_callback(void (*cb)(int level, const char *msg)) {
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
