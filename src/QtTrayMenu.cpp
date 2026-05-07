#include "QtTrayMenu.h"

#include <QApplication>

int argc = 1;
char *argvArray[] = {(char *) "TrayMenuApp", nullptr};
bool traydebug = false;

QtTrayMenu::QtTrayMenu():
    trayIcon(nullptr),
    trayStruct(nullptr),
    continueRunning(true),
    app(nullptr) {
  if (QApplication::instance()) {
    app = dynamic_cast<QApplication *>(QApplication::instance());
    if (!app) {
      fprintf(stderr, "QCoreApplication is not a QApplication, please contact support.");
    }
  } else {
    app = new QApplication(argc, &argvArray[0]);
  }
  if (traydebug) {
    app->installEventFilter(this);
  }
}

QtTrayMenu::~QtTrayMenu() {
  delete trayIcon;
  trayIcon = nullptr;  // Set to nullptr after deletion

  // Delete app only if it was created within this class
  if (app && app != QApplication::instance()) {
    delete app;
    app = nullptr;  // Set to nullptr after deletion
  }
}

int QtTrayMenu::init(struct tray *tray) {
  if (trayIcon) {
    return -1;  // Already initialized
  }

  this->trayStruct = tray;

  if (app->applicationName().isEmpty() || app->applicationName() == "TrayMenuApp") {
    app->setApplicationName(tray->tooltip);
  }

  trayIcon = new QSystemTrayIcon(QIcon(tray->icon));
  trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));

  connect(trayIcon, &QSystemTrayIcon::activated, this, &QtTrayMenu::onTrayActivated);
  connect(this, &QtTrayMenu::exitRequested, this, &QtTrayMenu::onExitRequested);

  auto *menu = new QMenu;
  createMenu(tray->menu, menu);

  trayIcon->setContextMenu(menu);
  trayIcon->show();

  return 0;
}

void QtTrayMenu::update(struct tray *tray) {
  this->trayStruct = tray;
  if (trayIcon) {
    auto newIcon = QIcon(tray->icon);
    if (!newIcon.isNull()) {
      trayIcon->setIcon(newIcon);
    }
    trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));
  }

  auto *existingMenu = trayIcon->contextMenu();
  if (existingMenu) {
    existingMenu->clear();  // Remove all actions
    createMenu(tray->menu, existingMenu);
  }
}

int QtTrayMenu::loop(int blocking) {
  if (!continueRunning) {
    return -1;
  }
  if (!app || app->closingDown()) {
    printf("Application is not in a valid state or is closing down.\n");
    return -1;
  }
  if (blocking) {
    app->exec();
    return -1;
  } else {
    app->processEvents();
    return 0;
  }
}

void QtTrayMenu::exit() {
  continueRunning = false;
  emit exitRequested();
}

void QtTrayMenu::createMenu(struct tray_menu *items, QMenu *menu) {
  auto separator = "-";
  while (items && items->text) {
    if (std::strcmp(items->text, separator) == 0) {
      menu->addSeparator();
    } else {
      auto *action = new QAction(QString::fromUtf8(items->text), menu);
      action->setDisabled(items->disabled == 1);
      action->setCheckable(items->checkbox == 1);
      action->setChecked(items->checked == 1);
      action->setProperty("tray_menu_item", QVariant::fromValue((void *) items));
      connect(action, &QAction::triggered, this, &QtTrayMenu::onMenuItemTriggered);
      if (items->submenu) {
        auto submenu = new QMenu;
        createMenu(items->submenu, submenu);
        action->setMenu(submenu);
      }
      menu->addAction(action);
    }
    items++;
  }
}

bool QtTrayMenu::eventFilter(QObject *watched, QEvent *event) {
  qDebug() << "Event Type:" << event->type();
  return QObject::eventFilter(watched, event);
}

void QtTrayMenu::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason == QSystemTrayIcon::Trigger && trayStruct->cb) {
    trayStruct->cb(trayStruct);
  }
}

void QtTrayMenu::onMenuItemTriggered() {
  auto *action = qobject_cast<QAction *>(sender());
  struct tray_menu *menuItem = getTrayMenuItem(action);

  if (menuItem && menuItem->cb) {
    menuItem->cb(menuItem);
  }
}

struct tray_menu *QtTrayMenu::getTrayMenuItem(QAction *action) {
  return (struct tray_menu *) action->property("tray_menu_item").value<void *>();
}

void QtTrayMenu::onExitRequested() {
  app->quit();
}
