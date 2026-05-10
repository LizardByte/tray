#include "QtTrayMenu.h"

#include <QApplication>
#include <QDebug>
#include <QMouseEvent>

QtTrayMenu::QtTrayMenu(QObject *parent):
    QtTrayMenu(-1, nullptr, false, parent) {
    };

QtTrayMenu::QtTrayMenu(int argc, char **argv, const bool debug, QObject *parent):
    QObject(parent) {
  if (QApplication::instance()) {
    app = dynamic_cast<QApplication *>(QApplication::instance());
    if (!app) {
      qDebug() << "QCoreApplication is not a QApplication, please contact support.";
    }
  } else {
    // Note: The following is ugly but QApplication requires an argv containing the application name.
    // We might not have access to the real argc/argv here due to being called/pulled as a dependency.
    if (argc < 0 && argv == nullptr) {
      argc = 1;
      char *argvArray[] = {(char *) "TrayMenuApp", nullptr};
      argv = &argvArray[0];
    }
    app = new QApplication(argc, argv);  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  }
  if (debug) {
    app->installEventFilter(this);
  }
}

QtTrayMenu::~QtTrayMenu() {
  // Delete app only if it was created within this class
  if (app && app != QApplication::instance()) {
    delete app;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    app = nullptr;  // Set to nullptr after deletion
  }

  // Remove custom references in correct order after app deletion to prevent SEGV
  delete trayIcon;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  trayIcon = nullptr;  // Set to nullptr after deletion

  delete trayTopMenu;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  trayTopMenu = nullptr;  // Set to nullptr after deletion
}

int QtTrayMenu::init(struct tray *tray) {
  if (trayIcon) {
    // Running tray is initialized again. Fail with error.
    return -1;
  }

  this->trayStruct = tray;
  this->running = true;

  if (QApplication::applicationName().isEmpty() || QApplication::applicationName() == "TrayMenuApp") {
    QApplication::setApplicationName(tray->tooltip);
  }

  trayIcon = new QSystemTrayIcon(QIcon(tray->icon), this);
  trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));

  connect(this, &QtTrayMenu::exitRequested, this, &QtTrayMenu::onExitRequested);
  connect(trayIcon, &QSystemTrayIcon::activated, this, &QtTrayMenu::onTrayActivated);
  connect(trayIcon, &QSystemTrayIcon::messageClicked, this, &QtTrayMenu::onMessageClicked);

  trayTopMenu = new QMenu();  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  createMenu(tray->menu, trayTopMenu);

  trayIcon->setContextMenu(trayTopMenu);
  trayIcon->show();

  createNotification();

  return 0;
}

void QtTrayMenu::update(struct tray *tray) {
  this->trayStruct = tray;
  if (trayIcon) {
    if (const auto newIcon = QIcon(tray->icon); !newIcon.isNull()) {
      trayIcon->setIcon(newIcon);
    }
    trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));
  }
  if (trayIcon == nullptr) {
    return;
  }
  if (auto *existingMenu = trayIcon->contextMenu()) {
    existingMenu->clear();  // Remove all actions
    createMenu(tray->menu, existingMenu);
  }
  createNotification();
}

int QtTrayMenu::loop(int blocking) const {
  if (!running) {
    return -1;
  }
  if (!app || QApplication::closingDown()) {
    qDebug() << "Application is not in a valid state or is closing down.";
    return -1;
  }
  if (blocking) {
    QApplication::exec();
    return -1;
  } else {
    QApplication::processEvents();
    return 0;
  }
}

void QtTrayMenu::exit() {
  running = false;
  emit exitRequested();
}

void QtTrayMenu::createMenu(struct tray_menu *items, QMenu *menu) {
  while (items && items->text) {
    if (strcmp(items->text, "-") == 0) {
      menu->addSeparator();
    } else {
      auto *action = new QAction(QString::fromUtf8(items->text), menu);
      action->setDisabled(items->disabled == 1);
      action->setCheckable(items->checkbox == 1);
      action->setChecked(items->checked == 1);
      action->setProperty("tray_menu_item", QVariant::fromValue((void *) items));
      connect(action, &QAction::triggered, this, &QtTrayMenu::onMenuItemTriggered);
      if (items->submenu) {
        const auto submenu = new QMenu(menu);
        createMenu(items->submenu, submenu);
        action->setMenu(submenu);
      }
      menu->addAction(action);
    }
    items++;
  }
}

void QtTrayMenu::createNotification() const {
  if (trayStruct->notification_title && trayStruct->notification_text) {
    const auto title = QString::fromUtf8(trayStruct->notification_title);
    const auto text = QString::fromUtf8(trayStruct->notification_text);
    if (trayStruct->notification_icon) {
      showMessage(title, text, QIcon(trayStruct->notification_icon));
    } else {
      showMessage(title, text);
    }
  }
}

bool QtTrayMenu::eventFilter(QObject *watched, QEvent *event) {
  qDebug() << "Event Type:" << event->type();
  return QObject::eventFilter(watched, event);
}

void QtTrayMenu::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason != QSystemTrayIcon::Trigger) {
    return;
  }
  if (trayStruct->cb) {
    trayStruct->cb(trayStruct);
  } else {
    showMenu();
  }
}

void QtTrayMenu::onMenuItemTriggered() {
  auto *action = qobject_cast<QAction *>(sender());
  struct tray_menu *menuItem = getTrayMenuItem(action);

  if (menuItem && menuItem->cb) {
    menuItem->cb(menuItem);
  }
}

struct tray_menu *QtTrayMenu::getTrayMenuItem(QAction *action) {  // NOSONAR(cpp:S995) - Use as defined in function interface
  return static_cast<struct tray_menu *>(action->property("tray_menu_item").value<void *>());
}

void QtTrayMenu::onExitRequested() const {
  QApplication::quit();
}

void QtTrayMenu::onMessageClicked() const {
  if (trayStruct->notification_cb) {
    trayStruct->notification_cb();
  }
}

void QtTrayMenu::configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const {
  const QString effective_name = !appName.isEmpty() ? appName : QStringLiteral("tray");
  if (QApplication::applicationName().isEmpty()) {
    QApplication::setApplicationName(effective_name);
  }

  if (QApplication::applicationDisplayName().isEmpty()) {
    if (!appDisplayName.isEmpty()) {
      QApplication::setApplicationDisplayName(appDisplayName);
    } else {
      const QString display_name =
        (trayStruct != nullptr && trayStruct->tooltip != nullptr) ? QString::fromUtf8(trayStruct->tooltip) : effective_name;
      QApplication::setApplicationDisplayName(display_name);
    }
  }

  if (!QApplication::desktopFileName().isEmpty()) {
    return;
  }

  if (!desktopName.isEmpty()) {
    QApplication::setDesktopFileName(desktopName);
    return;
  }

  QString desktop_name = QApplication::applicationName();
  if (!desktop_name.endsWith(QStringLiteral(".desktop"))) {
    desktop_name += QStringLiteral(".desktop");
  }
  QApplication::setDesktopFileName(desktop_name);
}

void QtTrayMenu::showMenu() const {
  if (QMenu *menu = trayIcon->contextMenu(); menu != nullptr) {
    // Due to QTBUG-139921 this is currently not working on Linux/Wayland
    // with Qt-6.9+ unless menu has a transient parent (which we do not have here).
    menu->show();
  }
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, const QSystemTrayIcon::MessageIcon icon, const int msecs) const {
  trayIcon->showMessage(title, msg, icon, msecs);
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, const QIcon &icon, const int msecs) const {
  trayIcon->showMessage(title, msg, icon, msecs);
}

void QtTrayMenu::clickMenuItem(int index) const {
  const QMenu *menu = trayIcon->contextMenu();
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
}

void QtTrayMenu::clickMessage() const {
  emit trayIcon->messageClicked();
}
