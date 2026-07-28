/**
 * @file src/QtTrayMenu.cpp
 * @brief Definitions for Qt tray menu implemenation
 */
// standard includes
#include <filesystem>

// qt includes
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QMouseEvent>
#include <QStyle>

// local includes
#include "QtTrayMenu.h"

#if defined(_WIN32)
  #include "WindowsAppearance.h"
#endif

QtTrayMenu::QtTrayMenu(QObject *parent, const bool debug):
    QtTrayMenu(-1, nullptr, parent, debug) {
    };

QtTrayMenu::QtTrayMenu(int argc, char **argv, QObject *parent, const bool debug):
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
      app = new QApplication(defaultArgc, defaultArgv.data());  // NOSONAR(cpp:S5025): QApplication must remain alive through process teardown
    } else {
      app = new QApplication(argc, argv);  // NOSONAR(cpp:S5025): QApplication must remain alive through process teardown
    }
  }
#if defined(_WIN32)
  tray_qt::windows::configure_appearance(app);
#endif
  if (debug) {
    app->installEventFilter(this);
  }
}

QtTrayMenu::~QtTrayMenu() = default;

int QtTrayMenu::init(struct tray *tray, const bool notification) {
  if (trayIcon) {
    // Running tray is initialized again. Fail with error.
    return -1;
  }
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    // Qt does not support system tray. Fail with error.
    return -1;
  }

  this->trayStruct = tray;
  this->running = true;

  if (QApplication::applicationName().isEmpty() || QApplication::applicationName() == "TrayMenuApp") {
    QApplication::setApplicationName(tray->tooltip);
  }

  // Create tray icon
  trayIcon = std::make_unique<QSystemTrayIcon>(lookupIcon(tray->icon));
  trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));

  connect(trayIcon.get(), &QSystemTrayIcon::activated, this, &QtTrayMenu::onTrayActivated);
  connect(trayIcon.get(), &QSystemTrayIcon::messageClicked, this, &QtTrayMenu::onMessageClicked);
  connect(this, &QtTrayMenu::update, this, &QtTrayMenu::onUpdate);
  connect(this, &QtTrayMenu::exit, this, &QtTrayMenu::onExitRequested);
  connect(this, &QtTrayMenu::showMenu, this, &QtTrayMenu::onShowMenu);

  updateMenu(tray->menu);

  trayIcon->setContextMenu(trayTopMenu.get());
  trayIcon->show();

  if (notification) {
    createNotification();
  }

  return 0;
}

void QtTrayMenu::onUpdate(struct tray *tray, const bool notify) {
  if (!trayIcon) {
    return;
  }
  this->trayStruct = tray;
  if (const auto newIcon = lookupIcon(trayStruct->icon); !newIcon.isNull()) {
    trayIcon->setIcon(newIcon);
  }
  trayIcon->setToolTip(QString::fromUtf8(trayStruct->tooltip));

  updateMenu(trayStruct->menu);
  if (notify) {
    createNotification();
  }
}

int QtTrayMenu::loop(int blocking) {
  if (!running) {
    return -1;
  }
  if (!app || QApplication::closingDown()) {
    qDebug() << "Application is not in a valid state or is closing down.";
    return -1;
  }
  if (blocking) {
    blockingEventLoop = true;
    QApplication::exec();
    return -1;
  } else {
    blockingEventLoop = false;
    QApplication::processEvents();
    return 0;
  }
}

void QtTrayMenu::onExitRequested() {
  // Mark as no longer running
  running = false;
  // Remove tray menu references
  if (trayTopMenu) {
    trayTopMenu->hide();
    if (trayIcon) {
      trayIcon->setContextMenu(nullptr);
    }
    trayTopMenu.reset();
  }
  // Remove tray icon references;
  if (trayIcon) {
    trayIcon->hide();
    trayIcon.reset();
  }
  // Unset tray structure
  trayStruct = nullptr;

  // If we run in a blocking event loop break said loop by quitting the QApplication
  if (blockingEventLoop) {
    QApplication::quit();
  }
}

void QtTrayMenu::updateMenu(struct tray_menu *items) {
  // Create and setup new tray menu instance
  auto newTrayTopMenu = std::make_unique<QMenu>();
#if defined(_WIN32)
  connect(newTrayTopMenu.get(), &QMenu::aboutToShow, this, []() {
    tray_qt::windows::sync_color_scheme();
  });
#endif
  trayIcon->setContextMenu(newTrayTopMenu.get());
  // Fill new tray menu instance
  createMenu(items, newTrayTopMenu.get());
  trayTopMenu = std::move(newTrayTopMenu);
}

void QtTrayMenu::createMenu(struct tray_menu *items, QMenu *menu) {
  while (items && items->text) {
    if (strcmp(items->text, "-") == 0) {
      menu->addSeparator();
    } else {
      auto *action = menu->addAction(QString::fromUtf8(items->text));
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

void QtTrayMenu::createNotification() {
  if (trayStruct && trayStruct->notification_title && trayStruct->notification_text) {
    const auto title = QString::fromUtf8(trayStruct->notification_title);
    const auto text = QString::fromUtf8(trayStruct->notification_text);
    if (trayStruct->notification_icon) {
      showMessage(title, text, trayStruct->notification_icon, trayStruct->notification_cb);
    } else {
      showMessage(title, text, trayStruct->notification_cb);
    }
  }
}

QIcon QtTrayMenu::lookupIcon(QString icon) const {
  // Find icon for tray
  if (std::filesystem::exists(icon.toStdString())) {
    if (auto result = QIcon(icon); !result.isNull()) {
      return result;
    }
  }
  if (auto result = QIcon::fromTheme(icon); !result.isNull()) {
    return result;
  }
  return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

bool QtTrayMenu::eventFilter(QObject *watched, QEvent *event) {
  qDebug() << "Event Type:" << event->type();
  return QObject::eventFilter(watched, event);
}

void QtTrayMenu::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason != QSystemTrayIcon::Trigger) {
    return;
  }
  if (trayStruct && trayStruct->cb) {
    trayStruct->cb(trayStruct);
  } else {
    showMenu();
  }
}

void QtTrayMenu::onMenuItemTriggered() {
  const auto *action = qobject_cast<const QAction *>(sender());
  struct tray_menu *menuItem = getTrayMenuItem(action);

  if (menuItem && menuItem->cb) {
    menuItem->cb(menuItem);
  }
}

struct tray_menu *QtTrayMenu::getTrayMenuItem(const QAction *action) {
  return static_cast<struct tray_menu *>(action->property("tray_menu_item").value<void *>());
}

void QtTrayMenu::onMessageClicked() const {
  if (notificationCallback == nullptr) {
    return;
  }

  auto callback = std::move(notificationCallback);
  notificationCallback = nullptr;
  callback();
}

void QtTrayMenu::configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const {
  const QString effective_name = !appName.isEmpty() ? appName : QStringLiteral("tray");
  if (!appName.isEmpty() || QApplication::applicationName().isEmpty() || QApplication::applicationName() == QStringLiteral("TrayMenuApp")) {
    QApplication::setApplicationName(effective_name);
  }

  if (!appDisplayName.isEmpty()) {
    QApplication::setApplicationDisplayName(appDisplayName);
  } else if (QApplication::applicationDisplayName().isEmpty()) {
    const QString display_name =
      (trayStruct && trayStruct->tooltip) ? QString::fromUtf8(trayStruct->tooltip) : effective_name;
    QApplication::setApplicationDisplayName(display_name);
  }

  if (!desktopName.isEmpty()) {
    QApplication::setDesktopFileName(desktopName);
    return;
  }

  if (!QApplication::desktopFileName().isEmpty()) {
    return;
  }

  QString desktop_name = QApplication::applicationName();
  if (!desktop_name.endsWith(QStringLiteral(".desktop"))) {
    desktop_name += QStringLiteral(".desktop");
  }
  QApplication::setDesktopFileName(desktop_name);
}

void QtTrayMenu::onShowMenu() const {
  if (!trayIcon) {
    return;
  }
  if (QMenu *menu = trayIcon->contextMenu(); menu != nullptr) {
    // Due to QTBUG-139921 this is currently not working on Linux/Wayland
    // with Qt-6.9+ unless menu has a transient parent (which we do not have here).
    menu->popup(QCursor::pos());
  }
}

bool QtTrayMenu::supportsMessages() {
  return QSystemTrayIcon::supportsMessages();
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, std::function<void()> callback, const QSystemTrayIcon::MessageIcon icon, const int msecs) {
  if (!trayIcon) {
    return;
  }
  if (QSystemTrayIcon::supportsMessages()) {
    notificationCallback = std::move(callback);
    emit trayIcon->showMessage(title, msg, icon, msecs);
  }
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, const QString &iconPath, std::function<void()> callback, const int msecs) {
  if (!trayIcon) {
    return;
  }
  if (QSystemTrayIcon::supportsMessages()) {
    notificationCallback = std::move(callback);
    emit trayIcon->showMessage(title, msg, lookupIcon(iconPath), msecs);
  }
}

void QtTrayMenu::clickMenuItem(int index) const {
  if (!trayIcon) {
    return;
  }
  const QMenu *menu = trayIcon->contextMenu();
  if (!menu) {
    return;
  }
  const QList<QAction *> actions = menu->actions();
  if (index < 0 || index >= actions.size()) {
    return;
  }
  QAction *action = actions.at(index);
  if (!action || action->isSeparator() || action->menu() != nullptr || !action->isEnabled()) {
    return;
  }
  emit action->trigger();
}

void QtTrayMenu::clickMessage() const {
  if (!trayIcon) {
    return;
  }
  emit trayIcon->messageClicked();
}

void QtTrayMenu::clearMessageCallback() const {
  notificationCallback = nullptr;
}
