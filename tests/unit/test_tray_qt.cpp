// test includes
#include "tests/conftest.cpp"

// local includes
#include "src/tray.h"

// standard includes
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {
  int &menu_callback_count() {
    static int count = 0;
    return count;
  }

  int &notification_callback_count() {
    static int count = 0;
    return count;
  }

  int &log_callback_count() {
    static int count = 0;
    return count;
  }

  void menu_item_cb([[maybe_unused]] struct tray_menu *item) {
    menu_callback_count()++;
  }

  void notification_cb() {
    notification_callback_count()++;
  }

  void log_cb([[maybe_unused]] int level, [[maybe_unused]] const char *msg) {
    log_callback_count()++;
  }
}  // namespace

class TrayQtCoverageTest: public BaseTest {  // NOSONAR(cpp:S3656) - fixture members/methods are accessed by TEST_F-generated subclasses
protected:  // NOSONAR(cpp:S3656) - TEST_F requires protected fixture visibility
  void SetUp() override {
    BaseTest::SetUp();

    tray_set_log_callback(nullptr);
    tray_set_app_info(nullptr, nullptr, nullptr);

    menu_callback_count() = 0;
    notification_callback_count() = 0;
    log_callback_count() = 0;

    submenuItems = {{{.text = "Nested", .cb = menu_item_cb}, {.text = nullptr}}};

    menuItems = {{{.text = "Clickable", .cb = menu_item_cb}, {.text = "-"}, {.text = "Submenu", .submenu = submenuItems.data()}, {.text = "Disabled", .disabled = 1, .cb = menu_item_cb}, {.text = "Second Clickable", .cb = menu_item_cb}, {.text = nullptr}}};

    trayDataStorage.assign(sizeof(struct tray), std::byte {0});
    trayData = reinterpret_cast<struct tray *>(trayDataStorage.data());  // NOSONAR(cpp:S3630) - required to map a C flexible-array struct over raw storage
    trayData->icon = "icon.png";
    trayData->tooltip = "Qt Tray Coverage";
    trayData->notification_icon = nullptr;
    trayData->notification_text = nullptr;
    trayData->notification_title = nullptr;
    trayData->notification_cb = nullptr;
    trayData->menu = menuItems.data();

    const int iconPathCount = 0;
    std::memcpy(const_cast<int *>(&trayData->iconPathCount), &iconPathCount, sizeof(iconPathCount));  // NOSONAR(cpp:S859) - required to initialize const member in C struct allocated via raw buffer
  }

  void TearDown() override {
    if (trayRunning) {
      tray_exit();
      tray_loop(0);
      trayRunning = false;
    }

    tray_set_log_callback(nullptr);
    BaseTest::TearDown();
  }

  void InitTray() {
    const int initResult = tray_init(trayData);
    trayRunning = (initResult == 0);
    ASSERT_EQ(initResult, 0);
  }

  void PumpEvents(int iterations = 20) {
    for (int i = 0; i < iterations; i++) {
      tray_loop(0);
    }
  }

  bool trayRunning {false};
  std::array<struct tray_menu, 6> menuItems {};
  std::array<struct tray_menu, 2> submenuItems {};
  std::vector<std::byte> trayDataStorage {};
  struct tray *trayData = nullptr;
};

TEST_F(TrayQtCoverageTest, SimulateMenuClickSkipsNonTriggerableActions) {
  InitTray();

  tray_simulate_menu_item_click(-1);
  tray_simulate_menu_item_click(99);
  tray_simulate_menu_item_click(1);
  tray_simulate_menu_item_click(2);
  tray_simulate_menu_item_click(3);
  PumpEvents();

  EXPECT_EQ(menu_callback_count(), 0);

  tray_simulate_menu_item_click(0);
  tray_simulate_menu_item_click(4);
  PumpEvents();

  EXPECT_EQ(menu_callback_count(), 2);
}

TEST_F(TrayQtCoverageTest, ApiCallsAreNoOpsBeforeInit) {
  tray_update(trayData);
  tray_show_menu();
  tray_simulate_menu_item_click(0);
  tray_simulate_notification_click();
  PumpEvents();

  EXPECT_EQ(menu_callback_count(), 0);
  EXPECT_EQ(notification_callback_count(), 0);
}

TEST_F(TrayQtCoverageTest, SimulateMenuClickWithNullMenuDoesNothing) {
  trayData->menu = nullptr;
  InitTray();

  tray_simulate_menu_item_click(0);
  PumpEvents();

  EXPECT_EQ(menu_callback_count(), 0);
}

TEST_F(TrayQtCoverageTest, SetAppInfoAppliesExplicitMetadata) {
  tray_set_app_info("tray-qt-tests", "Tray Qt Tests", "tray-qt-tests.desktop");
  InitTray();

  // Trigger an update to exercise metadata-dependent tray code paths.
  trayData->tooltip = "Explicit metadata update";
  tray_update(trayData);
  PumpEvents();
}

TEST_F(TrayQtCoverageTest, SetAppInfoDefaultsUseFallbackValues) {
  tray_set_app_info(nullptr, nullptr, nullptr);
  trayData->tooltip = "Tooltip Display Name";
  InitTray();

  trayData->tooltip = "Fallback metadata update";
  tray_update(trayData);
  PumpEvents();
}

TEST_F(TrayQtCoverageTest, LogCallbackCanBeSetAndReset) {
  InitTray();
  tray_set_log_callback(log_cb);

  // The callback is currently installed; this update path should remain stable.
  trayData->tooltip = "Log callback installed";
  tray_update(trayData);
  PumpEvents();

  EXPECT_EQ(log_callback_count(), 0);

  tray_set_log_callback(nullptr);

  trayData->tooltip = "Log callback removed";
  tray_update(trayData);
  PumpEvents();

  EXPECT_EQ(log_callback_count(), 0);
}

TEST_F(TrayQtCoverageTest, TrayExitCausesLoopToReturnExitCode) {
  InitTray();

  tray_exit();
  const int loopResult = tray_loop(0);
  trayRunning = false;

  EXPECT_EQ(loopResult, -1);
}

TEST_F(TrayQtCoverageTest, UpdateMenuStateWithSameLayoutKeepsCallbacksWorking) {
  InitTray();

  menuItems[0].text = "Clickable Renamed";
  menuItems[0].disabled = 1;
  tray_update(trayData);
  PumpEvents();

  tray_simulate_menu_item_click(0);
  PumpEvents();
  EXPECT_EQ(menu_callback_count(), 0);

  menuItems[0].disabled = 0;
  tray_update(trayData);
  PumpEvents();

  tray_simulate_menu_item_click(0);
  PumpEvents();
  EXPECT_EQ(menu_callback_count(), 1);
}

TEST_F(TrayQtCoverageTest, ResolveTrayIconFromIconPathArray) {
  // Build a tray struct with iconPathCount/allIconPaths to exercise fallback icon resolution.
  const size_t iconCount = 2;
  const size_t bufSize = sizeof(struct tray) + iconCount * sizeof(const char *);
  std::vector<std::byte> buf(bufSize, std::byte {0});
  auto *iconPathTray = reinterpret_cast<struct tray *>(buf.data());  // NOSONAR(cpp:S3630) - reinterpret_cast is required to map a C flexible-array struct over raw storage

  iconPathTray->icon = "missing-icon-name";
  iconPathTray->tooltip = "Icon path fallback";
  iconPathTray->notification_icon = nullptr;
  iconPathTray->notification_text = nullptr;
  iconPathTray->notification_title = nullptr;
  iconPathTray->notification_cb = nullptr;
  iconPathTray->menu = menuItems.data();

  const auto countVal = static_cast<int>(iconCount);
  std::memcpy(const_cast<int *>(&iconPathTray->iconPathCount), &countVal, sizeof(countVal));  // NOSONAR(cpp:S859) - const member initialization is required for this C interop allocation pattern
  const char *badIcon = "missing-icon-name";
  const char *goodIcon = "icon.png";
  std::memcpy(const_cast<char **>(&iconPathTray->allIconPaths[0]), &badIcon, sizeof(badIcon));  // NOSONAR(cpp:S859) - required to initialize const flexible-array entries
  std::memcpy(const_cast<char **>(&iconPathTray->allIconPaths[1]), &goodIcon, sizeof(goodIcon));  // NOSONAR(cpp:S859) - required to initialize const flexible-array entries

  const int initResult = tray_init(iconPathTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  tray_update(iconPathTray);
  PumpEvents();
}

TEST_F(TrayQtCoverageTest, NotificationWithoutCallbackDoesNotInvokeOnSimulation) {
  InitTray();

  trayData->notification_title = "No callback notification";
  trayData->notification_text = "Should not invoke callback";
  trayData->notification_icon = "icon.png";
  trayData->notification_cb = nullptr;

  tray_update(trayData);
  PumpEvents();

  tray_simulate_notification_click();
  PumpEvents();

  EXPECT_EQ(notification_callback_count(), 0);

  trayData->notification_title = nullptr;
  trayData->notification_text = nullptr;
  trayData->notification_icon = nullptr;
  tray_update(trayData);
  PumpEvents();
  waitForNativeNotificationTimeout();
}

TEST_F(TrayQtCoverageTest, ClearingNotificationDisablesSimulatedClickCallback) {
  InitTray();

  trayData->notification_title = "Qt Notification";
  trayData->notification_text = "Notification body";
  trayData->notification_icon = "mail-message-new";
  trayData->notification_cb = notification_cb;

  tray_update(trayData);
  PumpEvents();

  tray_simulate_notification_click();
  PumpEvents();
  EXPECT_EQ(notification_callback_count(), 1);

  trayData->notification_title = nullptr;
  trayData->notification_text = nullptr;
  trayData->notification_icon = nullptr;
  trayData->notification_cb = nullptr;

  tray_update(trayData);
  PumpEvents();

  tray_simulate_notification_click();
  PumpEvents();
  EXPECT_EQ(notification_callback_count(), 1);

  waitForNativeNotificationTimeout();
}
