/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <cstring>
#include <memory>

// local includes
#include "tray.h"

// Qt includes
#include <QApplication>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QScreen>
#include <QSystemTrayIcon>
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
  bool g_exit_pending = false;  // NOSONAR(cpp:S5421) - mutable state, not const
  uint g_notification_id = 0;  // NOSONAR(cpp:S5421) - tracks last D-Bus notification ID for proper cleanup
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

  /**
   * @brief Calculate the best position to show the context menu.
   *
   * Uses the tray icon geometry when available (reliable on X11/XEmbed and
   * some SNI desktops). Falls back to the current cursor position on systems
   * where the icon geometry cannot be determined. Qt's QMenu::popup() will
   * adjust the final position to keep the menu fully on-screen.
   *
   * @return The point at which to show the context menu.
   */
  QPoint calculateMenuPosition() {
    if (g_tray_icon != nullptr) {
      const QRect iconGeometry = g_tray_icon->geometry();
      if (iconGeometry.isValid()) {
        // Qt's popup() will flip the menu above the icon if it would go off-screen.
        return iconGeometry.bottomLeft();
      }
    }
    return QCursor::pos();
  }

  void close_notification() {
    if (g_notification_id == 0) {
      return;
    }
    QDBusInterface iface(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications")
    );
    if (iface.isValid()) {
      iface.call(QStringLiteral("CloseNotification"), g_notification_id);
    }
    g_notification_id = 0;
  }

  QMenu *build_menu(struct tray_menu *m, QWidget *parent) {
    auto *menu = new QMenu(parent);  // NOSONAR(cpp:S5025) - submenus owned by parent via Qt; top-level deleted manually
    for (; m != nullptr && m->text != nullptr; m++) {
      if (std::strcmp(m->text, "-") == 0) {
        menu->addSeparator();
      } else if (m->submenu != nullptr) {
        QMenu *sub = build_menu(m->submenu, menu);
        sub->setTitle(QString::fromUtf8(m->text));
        menu->addMenu(sub);
      } else {
        auto *action = menu->addAction(QString::fromUtf8(m->text));
        action->setEnabled(m->disabled == 0);
        if (m->checkbox) {
          action->setCheckable(true);
          action->setChecked(m->checked != 0);
        }
        if (m->cb != nullptr) {
          struct tray_menu *item = m;
          QObject::connect(action, &QAction::triggered, [item]() {
            item->cb(item);
          });
        }
      }
    }
    return menu;
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
      delete menu;  // NOSONAR(cpp:S5025) - QSystemTrayIcon does not own the context menu
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

    destroy_tray();
    g_loop_result = 0;
    g_exit_pending = false;

    g_tray_icon = new QSystemTrayIcon();  // NOSONAR(cpp:S5025) - raw pointer; deleted in destroy_tray() before QApplication

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      destroy_tray();
      return -1;
    }

    // Show the context menu on the default trigger (clicked).
    // QSystemTrayIcon::setContextMenu only handles right-click on X11/XEmbed;
    // SNI-based desktops may not show the menu at all without this explicit connection.
    // The menu is positioned using the tray icon geometry rather than the cursor position,
    // which is unreliable on Wayland.
    QObject::connect(g_tray_icon, &QSystemTrayIcon::activated, [](QSystemTrayIcon::ActivationReason reason) {
      if (reason == QSystemTrayIcon::Trigger) {
        if (g_tray_icon != nullptr) {
          QMenu *menu = g_tray_icon->contextMenu();
          if (menu != nullptr) {
            menu->popup(calculateMenuPosition());
          }
        }
      }
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
    return 0;
  }

  int tray_loop(int blocking) {
    if (g_exit_pending) {
      g_exit_pending = false;
      destroy_tray();
      destroy_app();
      return g_loop_result;
    }

    if (blocking) {
      QApplication::exec();
      if (g_exit_pending) {
        g_exit_pending = false;
        destroy_tray();
        destroy_app();
      }
    } else {
      QApplication::processEvents();
    }
    return g_loop_result;
  }

  void tray_update(struct tray *tray) {
    if (g_tray_icon == nullptr) {
      return;
    }

    const QString icon_str = QString::fromUtf8(tray->icon);
    g_tray_icon->setIcon(
      QFileInfo(icon_str).exists() ? QIcon(icon_str) : QIcon::fromTheme(icon_str)
    );

    if (tray->tooltip != nullptr) {
      g_tray_icon->setToolTip(QString::fromUtf8(tray->tooltip));
    }

    if (tray->menu != nullptr) {
      // setContextMenu does not take ownership; delete the old menu before replacing it.
      QMenu *old_menu = g_tray_icon->contextMenu();
      QMenu *new_menu = build_menu(tray->menu, nullptr);  // NOSONAR(cpp:S5025) - deleted via old_menu path or on next update
      g_tray_icon->setContextMenu(new_menu);
      delete old_menu;  // NOSONAR(cpp:S5025) - required; Qt does not own this
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
        // Store the callback before calling Notify so tray_simulate_notification_click works
        // even when the notification daemon is unavailable and the D-Bus reply is invalid.
        if (g_notification_handler != nullptr) {
          g_notification_handler->cb = tray->notification_cb;
        }
        QDBusReply<uint> reply = iface.call(
          QStringLiteral("Notify"),
          QStringLiteral("tray"),
          static_cast<uint>(0),
          icon,
          title,
          text,
          actions,
          hints,
          5000
        );
        if (reply.isValid()) {
          g_notification_id = reply.value();
          if (g_notification_handler != nullptr) {
            g_notification_handler->notification_id = g_notification_id;
          }
        }
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
  }

  void tray_show_menu(void) {
    if (g_tray_icon != nullptr) {
      QMenu *menu = g_tray_icon->contextMenu();
      if (menu != nullptr) {
        menu->popup(calculateMenuPosition());
        QApplication::processEvents();
      }
    }
  }

  void tray_simulate_notification_click(void) {
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
  }

  void tray_exit(void) {
    g_loop_result = -1;
    g_exit_pending = true;
    if (g_app_owned && QApplication::instance() != nullptr) {
      QApplication::quit();
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

}  // extern "C"

// Must be included at the end of a .cpp file when Q_OBJECT classes are defined
// in that .cpp (not in a header). AUTOMOC sees this directive and generates
// tray_linux.moc, which is then inlined here at compile time.
#include "tray_linux.moc"
