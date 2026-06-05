/**
 * @file src/QtTrayMenu.h
 * @brief Declarations for Qt tray menu implemenation
 */
#ifndef TRAYMENU_H
#define TRAYMENU_H

// qt includes
#include <QMenu>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

// local includes
#include "tray.h"

/**
 * @brief Wrapper class for platfrom-independent Qt-based tray menu.
 */
class QtTrayMenu: public QObject {
  Q_OBJECT

public:
  /**
   * @brief Create a QtTrayMenu instance
   * @param parent optional parent Qt object
   * @param debug if true isntall eventFilter for debug logging
   */
  explicit QtTrayMenu(QObject *parent = nullptr, bool debug = false);

  /**
   * @brief Create a QtTrayMenu instance
   * @param argc argument count for QApplication (if that needs to be created)
   * @param argv argument list for QApplication (if that needs to be created)
   * @param parent optional parent Qt object
   * @param debug if true isntall eventFilter for debug logging
   */
  explicit QtTrayMenu(int argc, char **argv, QObject *parent = nullptr, bool debug = false);

  ~QtTrayMenu() override;
  /**
   * @brief QObject override to filter events on watched object
   * @param watched object watched for event
   * @param event event on object
   * @return true if event should be filtered out and not be processed further
   * @see https://doc.qt.io/qt-6/qobject.html#eventFilter
   */
  bool eventFilter(QObject *watched, QEvent *event) override;

  /**
   * @brief Initialize tray with given structure
   * @param tray struct containing tray configuration
   * @param notification fire tray notification if true
   * @return 0 on success
   */
  int init(struct tray *tray, bool notification = true);

  /**
   * @brief Process tray loop events
   * @param blocking if true the function call will block until QtTrayMenu exits
   * @return 0 on successful processing if non-blocking, -1 otherwise
   */
  int loop(int blocking);

  /**
   * @brief Configure metadata for QApplication
   * @param appName the applications name
   * @param appDisplayName the applications display name
   * @param desktopName the applications desktop file name
   */
  void configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const;

  /**
   * @brief Show tray message popup
   * @param title popup title
   * @param msg popup message
   * @param callback tray message callback function
   * @param icon popup icon
   * @param msecs popup display duration
   */
  void showMessage(const QString &title, const QString &msg, std::function<void()> callback = nullptr, QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information, int msecs = 10000);

  /**
   * @brief Show tray message popup
   * @param title popup title
   * @param msg popup message
   * @param callback tray message callback function
   * @param iconPath popup icon file path
   * @param msecs popup display duration
   */
  void showMessage(const QString &title, const QString &msg, const QString &iconPath, std::function<void()> callback = nullptr, int msecs = 10000);

  /**
   * @brief Simulate click on menu item
   * @param index Menu item index to simulate click on
   */
  void clickMenuItem(int index) const;

  /**
   * @brief Simulate click on popup message
   */
  void clickMessage() const;

  /**
   * @brief Clear the stored popup message callback
   */
  void clearMessageCallback() const;

  /**
   * @brief Check if QtTrayMenu supports messages
   * @return true if messages can be shown
   */
  static bool supportsMessages();

signals:
  /**
   * @brief Exit tray and cleanup resources
   */
  void exit();

  /**
   * @brief Update tray configuration
   * @param tray struct containing tray configuration
   * @param notify fire tray notification if true
   */
  void update(struct tray *tray, bool notify = true);

  /**
   * @brief Show tray context menu
   */
  void showMenu() const;

private:
  void createMenu(struct tray_menu *items, QMenu *menu);
  void createNotification();
  void updateMenu(struct tray_menu *items);
  QIcon lookupIcon(QString icon) const;
  QApplication *app = nullptr;
  QSystemTrayIcon *trayIcon = nullptr;
  QMenu *trayTopMenu = nullptr;
  struct tray *trayStruct = nullptr;
  bool running = false;
  bool blockingEventLoop = false;
  struct tray_menu *getTrayMenuItem(QAction *action);
  mutable std::function<void()> notificationCallback = nullptr;

private slots:
  void onExitRequested();
  void onMessageClicked() const;
  void onMenuItemTriggered();
  void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
  void onShowMenu() const;
  void onUpdate(struct tray *tray, bool notify);
};
#endif  // TRAYMENU_H
