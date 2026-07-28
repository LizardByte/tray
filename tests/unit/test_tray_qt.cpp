// test includes
#include "tests/conftest.cpp"
#include "tests/notification_utils.h"

// local includes
#include "src/tray.h"

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#if defined(_WIN32)
  // local includes
  #include "src/WindowsAppearance.h"
#endif

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

class TrayQtCoverageTest: public BaseTest {
private:
  bool trayRunning_ {false};
  std::array<struct tray_menu, 6> menuItems_ {};
  std::array<struct tray_menu, 2> submenuItems_ {};
  std::vector<std::byte> trayDataStorage_ {};
  struct tray *trayData_ = nullptr;

protected:
  bool &trayRunning = trayRunning_;
  std::array<struct tray_menu, 6> &menuItems = menuItems_;
  std::array<struct tray_menu, 2> &submenuItems = submenuItems_;
  std::vector<std::byte> &trayDataStorage = trayDataStorage_;
  struct tray *&trayData = trayData_;

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
    trayData = ::new (static_cast<void *>(trayDataStorage.data())) tray {
      .icon = "icon.png",
      .tooltip = "Qt Tray Coverage",
      .notification_icon = nullptr,
      .notification_text = nullptr,
      .notification_title = nullptr,
      .notification_cb = nullptr,
      .cb = nullptr,
      .menu = menuItems.data(),
      .iconPathCount = 0,
    };
  }

  void TearDown() override {
    if (trayRunning) {
      tray_exit();
      tray_loop(0);
      trayRunning = false;
    }

    tray_restore_mouse_position();
    tray_set_log_callback(nullptr);
    BaseTest::TearDown();
  }

  void InitTray() {
    const int initResult = tray_init(trayData);
    trayRunning = (initResult == 0);
    ASSERT_EQ(initResult, 0);
  }

  void PumpEvents(int iterations = 20) const {
    for (int i = 0; i < iterations; i++) {
      tray_loop(0);
    }
  }
};

#if defined(_WIN32)
TEST(WindowsAppearanceTest, AppsUseLightThemeMapsToColorScheme) {
  using tray_qt::windows::color_scheme_e;
  using tray_qt::windows::color_scheme_from_apps_use_light_theme;

  EXPECT_EQ(color_scheme_from_apps_use_light_theme(std::nullopt), color_scheme_e::unknown);
  EXPECT_EQ(color_scheme_from_apps_use_light_theme(std::uint32_t {0}), color_scheme_e::dark);
  EXPECT_EQ(color_scheme_from_apps_use_light_theme(std::uint32_t {1}), color_scheme_e::light);
  EXPECT_EQ(color_scheme_from_apps_use_light_theme(std::uint32_t {2}), color_scheme_e::light);
}

TEST_F(TrayQtCoverageTest, MirrorsInteractiveUserWindowsAppearance) {
  const auto expected_color_scheme = tray_qt::windows::interactive_user_color_scheme();
  InitTray();

  if (expected_color_scheme != tray_qt::windows::color_scheme_e::unknown) {
    EXPECT_EQ(tray_qt::windows::current_color_scheme(), expected_color_scheme);
  }
  if (tray_qt::windows::should_use_windows_11_style()) {
    EXPECT_TRUE(tray_qt::windows::windows_11_style_is_active());
  }
}
#endif

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
  EXPECT_EQ(tray_position_mouse_over_icon(), -1);
  EXPECT_EQ(tray_restore_mouse_position(), -1);
  tray_simulate_menu_item_click(0);
  tray_simulate_notification_click();
  PumpEvents();

  EXPECT_EQ(menu_callback_count(), 0);
  EXPECT_EQ(notification_callback_count(), 0);
}

TEST_F(TrayQtCoverageTest, UpdateFromWorkerThreadWaitsForApplicationThread) {
  InitTray();

  std::array<struct tray_menu, 2> workerMenu = {{{.text = "Worker item", .cb = menu_item_cb}, {.text = nullptr}}};
  std::array<struct tray_menu, 1> emptyMenu = {{{.text = nullptr}}};
  trayData->menu = workerMenu.data();

  std::atomic workerStarted {false};
  std::atomic updateReturned {false};
  std::thread worker([this, &emptyMenu, &updateReturned, &workerStarted]() {
    workerStarted.store(true);
    tray_update(trayData);
    trayData->menu = emptyMenu.data();
    updateReturned.store(true);
  });

  while (!workerStarted.load()) {
    std::this_thread::yield();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(updateReturned.load());

  while (!updateReturned.load()) {
    PumpEvents(1);
  }
  worker.join();

  // Process any asynchronously queued update before checking the resulting menu.
  PumpEvents();
  tray_simulate_menu_item_click(0);
  PumpEvents();
  EXPECT_EQ(menu_callback_count(), 1);
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
  const auto countVal = static_cast<int>(iconCount);
  auto *iconPathTray = ::new (static_cast<void *>(buf.data())) tray {
    .icon = "missing-icon-name",
    .tooltip = "Icon path fallback",
    .notification_icon = nullptr,
    .notification_text = nullptr,
    .notification_title = nullptr,
    .notification_cb = nullptr,
    .cb = nullptr,
    .menu = menuItems.data(),
    .iconPathCount = countVal,
  };
  const char *badIcon = "missing-icon-name";
  const char *goodIcon = "icon.png";
  iconPathTray->allIconPaths[0] = badIcon;
  iconPathTray->allIconPaths[1] = goodIcon;

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
