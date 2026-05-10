#ifndef TRAYMENU_H
#define TRAYMENU_H

#include "tray.h"

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

/**
 * @brief Wrapper class for platfrom-independent Qt-based tray menu.
 */
class QtTrayMenu: public QObject {
  Q_OBJECT

public:
  /**
   * @brief Create a QtTrayMenu instance
   * @param parent optional parent Qt object
   */
  explicit QtTrayMenu(QObject *parent = nullptr);

  /**
   * @brief Create a QtTrayMenu instance
   * @param argc argument count for QApplication (if that needs to be created)
   * @param argv argument list for QApplication (if that needs to be created)
   * @param debug if true isntall eventFilter for debug logging
   * @param parent optional parent Qt object
   */
  explicit QtTrayMenu(int argc, char **argv, bool debug, QObject *parent = nullptr);

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
   * @return 0 on success
   */
  int init(struct tray *tray);

  /**
   * @brief Update tray configuration
   * @param tray struct containing tray configuration
   */
  void update(struct tray *tray);

  /**
   * @brief Process tray loop events
   * @param blocking if true the function call will block until QtTrayMenu exits
   * @return 0 on successful processing if non-blocking, -1 otherwise
   */
  int loop(int blocking) const;

  /**
   * @brief Initialize tray with given structure
   */
  void exit();

  /**
   * @brief Configure metadata for QApplication
   * @param appName the applications name
   * @param appDisplayName the applications display name
   * @param desktopName the applications desktop file name
   */
  void configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const;

  /**
   * @brief Show tray context menu
   */
  void showMenu() const;

  /**
   * @brief Show tray message popup
   * @param title popup title
   * @param msg popup message
   * @param icon popup icon
   * @param msecs popup display duration
   */
  void showMessage(const QString &title, const QString &msg, QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information, int msecs = 10000) const;

  /**
   * @brief Show tray message popup
   * @param title popup title
   * @param msg popup message
   * @param icon popup icon
   * @param msecs popup display duration
   */
  void showMessage(const QString &title, const QString &msg, const QIcon &icon, int msecs = 10000) const;

  /**
   * @brief Simulate click on menu item
   * @param index Menu item index to simulate click on
   */
  void clickMenuItem(int index) const;

  /**
   * @brief Simulate click on popup message
   */
  void clickMessage() const;

private:
  void createMenu(struct tray_menu *items, QMenu *menu);
  void createNotification() const;
  QApplication *app = nullptr;
  QSystemTrayIcon *trayIcon = nullptr;
  QMenu *trayTopMenu = nullptr;
  struct tray *trayStruct = nullptr;
  bool running = false;
  struct tray_menu *getTrayMenuItem(QAction *action);

private slots:
  void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
  void onMessageClicked() const;
  void onMenuItemTriggered();
};
#endif  // TRAYMENU_H
