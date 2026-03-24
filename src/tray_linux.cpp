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
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVariantMap>

namespace {
  std::unique_ptr<QApplication> g_app;  // NOSONAR(cpp:S5421) - mutable state, not const
  QSystemTrayIcon *g_tray_icon = nullptr;  // NOSONAR(cpp:S5421) - mutable state, not const
  int g_loop_result = 0;  // NOSONAR(cpp:S5421) - mutable state, not const
  bool g_app_owned = false;  // NOSONAR(cpp:S5421) - mutable state, not const
  bool g_exit_pending = false;  // NOSONAR(cpp:S5421) - mutable state, not const
  uint g_notification_id = 0;  // NOSONAR(cpp:S5421) - tracks last D-Bus notification ID for proper cleanup

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

    g_tray_icon->setIcon(QIcon(QString::fromUtf8(tray->icon)));

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
    QObject::disconnect(g_tray_icon, &QSystemTrayIcon::messageClicked, nullptr, nullptr);
    close_notification();
    if (!text.isEmpty()) {
      const QString title = tray->notification_title != nullptr ? QString::fromUtf8(tray->notification_title) : QString();
      const char *icon_path = tray->notification_icon != nullptr ? tray->notification_icon : tray->icon;
      QString icon;
      if (icon_path != nullptr) {
        icon = QUrl::fromLocalFile(QFileInfo(QString::fromUtf8(icon_path)).absoluteFilePath()).toString();
      }
      QVariantMap hints;
      if (!icon.isEmpty()) {
        hints[QStringLiteral("image-path")] = icon;
      }
      if (tray->notification_cb != nullptr) {
        void (*cb)() = tray->notification_cb;
        QObject::connect(g_tray_icon, &QSystemTrayIcon::messageClicked, [cb]() {
          cb();
        });
      }
      QDBusInterface iface(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications")
      );
      if (iface.isValid()) {
        QDBusReply<uint> reply = iface.call(
          QStringLiteral("Notify"),
          QStringLiteral("tray"),
          static_cast<uint>(0),
          icon,
          title,
          text,
          QStringList(),
          hints,
          5000
        );
        if (reply.isValid()) {
          g_notification_id = reply.value();
        }
      } else {
        g_tray_icon->showMessage(title, text, QSystemTrayIcon::Information, 5000);
      }
    }
  }

  void tray_show_menu(void) {
    if (g_tray_icon != nullptr) {
      QMenu *menu = g_tray_icon->contextMenu();
      if (menu != nullptr) {
        menu->popup(QCursor::pos());
        QApplication::processEvents();
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

}  // extern "C"
