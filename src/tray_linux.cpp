/**
 * @file src/tray_linux.cpp
 * @brief System tray implementation for Linux using Qt.
 */
// standard includes
#include <cstring>

// local includes
#include "tray.h"

// Qt includes
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

static QApplication *app = nullptr;
static QSystemTrayIcon *tray_icon = nullptr;
static QMenu *tray_menu = nullptr;
static int loop_result = 0;
static bool app_owned = false;

static QMenu *_tray_menu(struct tray_menu *m) {
  auto *menu = new QMenu();
  for (; m != nullptr && m->text != nullptr; m++) {
    if (std::strcmp(m->text, "-") == 0) {
      menu->addSeparator();
    } else if (m->submenu != nullptr) {
      QMenu *sub = _tray_menu(m->submenu);
      sub->setTitle(QString::fromUtf8(m->text));
      menu->addMenu(sub);
    } else if (m->checkbox) {
      auto *action = menu->addAction(QString::fromUtf8(m->text));
      action->setCheckable(true);
      action->setChecked(m->checked != 0);
      action->setEnabled(m->disabled == 0);
      if (m->cb != nullptr) {
        struct tray_menu *item = m;
        QObject::connect(action, &QAction::triggered, [item]() {
          item->cb(item);
        });
      }
    } else {
      auto *action = menu->addAction(QString::fromUtf8(m->text));
      action->setEnabled(m->disabled == 0);
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

extern "C" {

int tray_init(struct tray *tray) {
  if (QApplication::instance() == nullptr) {
    static int argc = 0;
    app = new QApplication(argc, nullptr);
    app_owned = true;
  }

  if (tray_icon != nullptr) {
    delete tray_icon;
    tray_icon = nullptr;
  }
  if (tray_menu != nullptr) {
    delete tray_menu;
    tray_menu = nullptr;
  }

  loop_result = 0;

  tray_icon = new QSystemTrayIcon();

  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    delete tray_icon;
    tray_icon = nullptr;
    return -1;
  }

  tray_update(tray);
  tray_icon->show();
  return 0;
}

int tray_loop(int blocking) {
  if (blocking) {
    QApplication::exec();
  } else {
    QApplication::processEvents();
  }
  return loop_result;
}

void tray_update(struct tray *tray) {
  if (tray_icon == nullptr) {
    return;
  }

  tray_icon->setIcon(QIcon(QString::fromUtf8(tray->icon)));

  if (tray->tooltip != nullptr) {
    tray_icon->setToolTip(QString::fromUtf8(tray->tooltip));
  }

  if (tray->menu != nullptr) {
    QMenu *new_menu = _tray_menu(tray->menu);
    tray_icon->setContextMenu(new_menu);
    if (tray_menu != nullptr) {
      delete tray_menu;
    }
    tray_menu = new_menu;
  }

  if (tray->notification_text != nullptr && std::strlen(tray->notification_text) > 0) {
    QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information;
    const QString title = tray->notification_title != nullptr ? QString::fromUtf8(tray->notification_title) : QString();
    const QString text = QString::fromUtf8(tray->notification_text);
    tray_icon->showMessage(title, text, icon, 5000);

    if (tray->notification_cb != nullptr) {
      void (*cb)() = tray->notification_cb;
      QObject::connect(tray_icon, &QSystemTrayIcon::messageClicked, [cb]() {
        cb();
      });
    }
  }
}

void tray_show_menu(void) {
  if (tray_icon != nullptr && tray_menu != nullptr) {
    tray_menu->popup(QCursor::pos());
    QApplication::processEvents();
  }
}

void tray_exit(void) {
  loop_result = -1;

  if (tray_icon != nullptr) {
    tray_icon->hide();
    delete tray_icon;
    tray_icon = nullptr;
  }

  if (tray_menu != nullptr) {
    delete tray_menu;
    tray_menu = nullptr;
  }

  if (app_owned && app != nullptr) {
    app->quit();
  }
}

}  // extern "C"
