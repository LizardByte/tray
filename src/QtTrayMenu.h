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
  explicit QtTrayMenu(QObject *parent = nullptr);
  explicit QtTrayMenu(int argc, char **argv, bool debug, QObject *parent = nullptr);
  ~QtTrayMenu() override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  int init(struct tray *tray);
  void update(struct tray *tray);
  int loop(int blocking) const;
  void exit();
  void configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const;
  void showMenu() const;
  void showMessage(const QString &title, const QString &msg, QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information, int msecs = 10000) const;
  void showMessage(const QString &title, const QString &msg, const QIcon &icon, int msecs = 10000) const;
  void clickMenuItem(int index) const;
  void clickMessage() const;

private:
  void createMenu(struct tray_menu *items, QMenu *menu);
  void createNotification() const;
  QApplication *app;
  QSystemTrayIcon *trayIcon;
  QMenu *trayTopMenu;
  struct tray *trayStruct;
  bool continueRunning;
  struct tray_menu *getTrayMenuItem(QAction *action);

signals:
  void exitRequested();

private slots:
  void onExitRequested() const;
  void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
  void onMessageClicked() const;
  void onMenuItemTriggered();
};
#endif  // TRAYMENU_H
