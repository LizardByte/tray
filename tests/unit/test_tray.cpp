// test includes
#include "tests/conftest.cpp"

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <new>
#include <ostream>
#include <string>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>

#if defined(_WIN32) || defined(_WIN64)
  #include <Windows.h>
#endif

// local includes
#include "src/tray.h"

// test includes
#include "tests/notification_utils.h"
#include "tests/screenshot_utils.h"

constexpr const char *TRAY_ICON_ICO = "icon.ico";
constexpr const char *TRAY_ICON_PNG = "icon.png";
constexpr const char *TRAY_ICON_SVG = "icon.svg";
constexpr const char *TRAY_ICON2_ICO = "icon2.ico";
constexpr const char *TRAY_ICON2_PNG = "icon2.png";
constexpr const char *TRAY_ICON2_SVG = "icon2.svg";
constexpr const char *TRAY_ICON_THEMED = "mail-message-new";
constexpr const char *TRAY_ICON1 = TRAY_ICON_PNG;
constexpr const char *TRAY_ICON2 = TRAY_ICON2_PNG;

// File-scope tray data shared across all TrayTest instances
namespace {
  struct TrayIconParam {
    const char *name;
    const char *icon;
    const char *alternateIcon;
  };

  constexpr std::array<TrayIconParam, 4> TRAY_ICON_PARAMS {
    {{"svg", TRAY_ICON_SVG, TRAY_ICON2_SVG},
     {"ico", TRAY_ICON_ICO, TRAY_ICON2_ICO},
     {"png", TRAY_ICON_PNG, TRAY_ICON2_PNG},
     {"themed", TRAY_ICON_THEMED, TRAY_ICON_THEMED}}
  };

  std::string trayIconParamName(const ::testing::TestParamInfo<TrayIconParam> &info) {
    return info.param.name;
  }

  void PrintTo(const TrayIconParam &param, std::ostream *os) {
    *os << param.name;
  }

  std::string nativeNotificationSkipReason() {
#if defined(_WIN32)
    QUERY_USER_NOTIFICATION_STATE notification_state;
    if (const HRESULT ns = SHQueryUserNotificationState(&notification_state); ns != S_OK || notification_state != QUNS_ACCEPTS_NOTIFICATIONS) {
      return "Notifications not accepted in this environment. SHQueryUserNotificationState result: " + std::to_string(ns) + ", state: " + std::to_string(notification_state);
    }
#endif

    return {};
  }

}  // namespace

class TrayTest: public BaseTest {
private:
  static TrayTest &fixtureFor(struct tray_menu *item) {
    return *static_cast<TrayTest *>(item->context);
  }

  std::array<struct tray_menu, 4> submenu7_8_ {{{.text = "7", .cb = submenu_cb, .context = this}, {.text = "-"}, {.text = "8", .cb = submenu_cb, .context = this}, {.text = nullptr}}};
  std::array<struct tray_menu, 3> submenu5_6_ {{{.text = "5", .cb = submenu_cb, .context = this}, {.text = "6", .cb = submenu_cb, .context = this}, {.text = nullptr}}};
  std::array<struct tray_menu, 3> submenuSecond_ {{{.text = "THIRD", .context = this, .submenu = submenu7_8_.data()}, {.text = "FOUR", .context = this, .submenu = submenu5_6_.data()}, {.text = nullptr}}};
  std::array<struct tray_menu, 8> submenu_ {{{.text = "Hello", .cb = hello_cb, .context = this}, {.text = "Checked", .checked = 1, .checkbox = 1, .cb = toggle_cb, .context = this}, {.text = "Disabled", .disabled = 1}, {.text = "-"}, {.text = "SubMenu", .context = this, .submenu = submenuSecond_.data()}, {.text = "-"}, {.text = "Quit", .cb = quit_cb, .context = this}, {.text = nullptr}}};
  std::array<std::byte, sizeof(struct tray)> testTrayStorage_ {};
  struct tray *testTray_ = ::new (static_cast<void *>(testTrayStorage_.data())) tray {
    .icon = TRAY_ICON1,
    .tooltip = "TestTray",
    .menu = submenu_.data(),
    .iconPathCount = 0,
  };
  bool trayRunning_ {false};

protected:
  bool &trayRunning = trayRunning_;
  struct tray &testTray = *testTray_;
  struct tray_menu *const submenu = submenu_.data();
  struct tray_menu *const submenu7_8 = submenu7_8_.data();
  struct tray_menu *const submenu5_6 = submenu5_6_.data();
  struct tray_menu *const submenu_second = submenuSecond_.data();

  void ShutdownTray() {
    if (!trayRunning) {
      return;
    }

    // Ensure per-test notification state is cleared before teardown so
    // screenshot tests do not inherit prior notification popups.
    testTray.notification_title = nullptr;
    testTray.notification_text = nullptr;
    testTray.notification_icon = nullptr;
    testTray.notification_cb = nullptr;
    tray_update(&testTray);
    for (int i = 0; i < 20; ++i) {
      tray_loop(0);
    }

    tray_exit();
    tray_loop(0);
    trayRunning = false;
  }

  // Dismisses the open menu from a background thread.
  void closeMenu() const {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Capture a screenshot while the tray menu is open, then dismiss and exit.
  void captureMenuStateAndExit(const char *screenshotName) const {
    const bool positionMouse = lizardbyte::common::is_github_actions();
    int positionMouseResult = -1;
    if (positionMouse) {
      WaitForTrayReady();
      positionMouseResult = tray_position_mouse_over_icon();
      EXPECT_EQ(positionMouseResult, 0);
    }

    std::atomic_bool exitRequested {false};
    std::thread capture_thread([this, screenshotName, &exitRequested]() {  // NOSONAR(cpp:S6168): C++17 has no std::jthread and this thread is explicitly joined
      EXPECT_TRUE(captureScreenshot(screenshotName));
      closeMenu();
      exitRequested.store(true, std::memory_order_release);
    });

    tray_show_menu();
    while (tray_loop(0) == 0) {
      if (exitRequested.load(std::memory_order_acquire)) {
        tray_exit();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    capture_thread.join();
    if (positionMouse) {
      const int restoreMouseResult = tray_restore_mouse_position();
      if (positionMouseResult == 0) {
        EXPECT_EQ(restoreMouseResult, 0);
      }
    }
  }

  static void hello_cb(struct tray_menu *) {
    // Mock implementation
  }

  static void toggle_cb(struct tray_menu *item) {
    auto &fixture = fixtureFor(item);
    item->checked = !item->checked;
    tray_update(fixture.testTray_);
  }

  static void quit_cb(struct tray_menu *) {
    tray_exit();
  }

  static void submenu_cb(struct tray_menu *item) {
    // Mock implementation
    tray_update(fixtureFor(item).testTray_);
  }

  void SetUp() override {
    BaseTest::SetUp();

    // Skip tests if screenshot tooling is not available
    if (!ensureScreenshotReady()) {
      GTEST_SKIP() << "Screenshot tooling missing: " << screenshotUnavailableReason();
    }
    if (screenshot::output_root().empty()) {
      GTEST_SKIP() << "Screenshot output path not initialized";
    }

    // Ensure icon files exist in test binary directory
    std::filesystem::path projectRoot = testBinaryDir().parent_path();
    auto ensureIconInTestDir = [&projectRoot, this](const char *iconName) {
      std::filesystem::path iconSource;

      if (std::filesystem::exists(projectRoot / "icons" / iconName)) {
        iconSource = projectRoot / "icons" / iconName;
      } else if (std::filesystem::exists(projectRoot / iconName)) {
        iconSource = projectRoot / iconName;
      } else if (std::filesystem::exists(std::filesystem::path(iconName))) {
        iconSource = std::filesystem::path(iconName);
      }

      if (!iconSource.empty()) {
        std::filesystem::path iconDest = testBinaryDir() / iconName;
        if (!std::filesystem::exists(iconDest)) {
          std::error_code ec;
          std::filesystem::copy_file(iconSource, iconDest, ec);
          if (ec) {
            std::cout << "Warning: Failed to copy icon file: " << ec.message() << std::endl;
          }
        }
      }
    };

    auto ensureFileIconInTestDir = [&ensureIconInTestDir](const char *iconName) {
      if (std::filesystem::path(iconName).has_extension()) {
        ensureIconInTestDir(iconName);
      }
    };

    for (const auto &iconParam : TRAY_ICON_PARAMS) {
      ensureFileIconInTestDir(iconParam.icon);
      ensureFileIconInTestDir(iconParam.alternateIcon);
    }

    trayRunning = false;
    testTray.icon = TRAY_ICON1;
    testTray.tooltip = "TestTray";
    testTray.notification_title = nullptr;
    testTray.notification_text = nullptr;
    testTray.notification_icon = nullptr;
    testTray.notification_cb = nullptr;
    testTray.menu = submenu;
    submenu[1].checked = 1;
  }

  void TearDown() override {
    ShutdownTray();
    tray_restore_mouse_position();
    BaseTest::TearDown();
  }

  // Process pending events to allow tray icon to appear.
  // Call this ONLY before screenshots to ensure the icon is visible.
  void WaitForTrayReady() const {
    for (int i = 0; i < 100; i++) {
      tray_loop(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void WaitForNotificationReady() const {
    WaitForTrayReady();
#if defined(_WIN32) || defined(__APPLE__)
    if (lizardbyte::common::is_github_actions()) {
      for (int i = 0; i < 40; i++) {
        tray_loop(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
#endif
  }
};

class TrayIconTest:
    public TrayTest,
    public ::testing::WithParamInterface<TrayIconParam> {};

class TrayNotificationIconTest:
    public TrayTest,
    public ::testing::WithParamInterface<TrayIconParam> {};

TEST_F(TrayTest, TestTrayInit) {
  int result = tray_init(&testTray);
  trayRunning = (result == 0);
  EXPECT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_initial"));
}

TEST_P(TrayIconTest, TestTrayIconDisplay) {
  const auto &iconParam = GetParam();
  testTray.icon = iconParam.icon;

  int result = tray_init(&testTray);
  trayRunning = (result == 0);
  EXPECT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot(std::string("tray_icon_") + iconParam.name));
}

TEST_F(TrayTest, TestTrayLoop) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);
  // Test non-blocking loop (blocking=0) since blocking would hang without events
  int result = tray_loop(0);
  EXPECT_EQ(result, 0);
}

TEST_F(TrayTest, TestTrayUpdate) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);
  EXPECT_EQ(testTray.icon, TRAY_ICON1);

  // update the values
  testTray.icon = TRAY_ICON2;
  testTray.tooltip = "TestTray2";
  tray_update(&testTray);
  EXPECT_EQ(testTray.icon, TRAY_ICON2);

  // put back the original values
  testTray.icon = TRAY_ICON1;
  testTray.tooltip = "TestTray";
  tray_update(&testTray);
  EXPECT_EQ(testTray.icon, TRAY_ICON1);
  EXPECT_EQ(testTray.tooltip, "TestTray");
}

TEST_F(TrayTest, TestToggleCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);
  bool initialCheckedState = testTray.menu[1].checked;
  toggle_cb(&testTray.menu[1]);
  EXPECT_EQ(testTray.menu[1].checked, !initialCheckedState);
}

TEST_F(TrayTest, TestMenuItemCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test hello callback - it should work without crashing
  ASSERT_NE(testTray.menu[0].cb, nullptr);
  testTray.menu[0].cb(&testTray.menu[0]);
}

TEST_F(TrayTest, TestDisabledMenuItem) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify disabled menu item
  EXPECT_EQ(testTray.menu[2].disabled, 1);
  EXPECT_STREQ(testTray.menu[2].text, "Disabled");
}

TEST_F(TrayTest, TestMenuSeparator) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify separator exists
  EXPECT_STREQ(testTray.menu[3].text, "-");
  EXPECT_EQ(testTray.menu[3].cb, nullptr);
}

TEST_F(TrayTest, TestSubmenuStructure) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify submenu structure
  EXPECT_STREQ(testTray.menu[4].text, "SubMenu");
  ASSERT_NE(testTray.menu[4].submenu, nullptr);

  // Verify nested submenu levels
  EXPECT_STREQ(testTray.menu[4].submenu[0].text, "THIRD");
  ASSERT_NE(testTray.menu[4].submenu[0].submenu, nullptr);
  EXPECT_STREQ(testTray.menu[4].submenu[0].submenu[0].text, "7");
}

TEST_F(TrayTest, TestSubmenuCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test submenu callback
  ASSERT_NE(testTray.menu[4].submenu[0].submenu[0].cb, nullptr);
  testTray.menu[4].submenu[0].submenu[0].cb(&testTray.menu[4].submenu[0].submenu[0]);
}

TEST_P(TrayNotificationIconTest, TestNotificationDisplay) {
  if (const std::string skipReason = nativeNotificationSkipReason(); !skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }

  const auto &iconParam = GetParam();
  testTray.icon = iconParam.icon;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);
  dismissNativeNotifications();

  // Set notification properties
  testTray.notification_title = "Test Notification";
  testTray.notification_text = "This is a test notification message";
  testTray.notification_icon = iconParam.icon;

  tray_update(&testTray);

  WaitForNotificationReady();
  EXPECT_TRUE(captureScreenshot(std::string("tray_notification_") + iconParam.name + "_icon"));

  // Clear notification
  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  tray_update(&testTray);
  waitForNativeNotificationTimeout();
}

TEST_P(TrayNotificationIconTest, TestNotificationCallback) {
  if (const std::string skipReason = nativeNotificationSkipReason(); !skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }

  const auto &iconParam = GetParam();
  testTray.icon = iconParam.icon;

  static bool callbackInvoked = false;
  auto notification_callback = []() {
    callbackInvoked = true;
  };

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Set notification with callback
  testTray.notification_title = "Clickable Notification";
  testTray.notification_text = "Click this notification to test callback";
  testTray.notification_icon = iconParam.icon;
  testTray.notification_cb = notification_callback;

  tray_update(&testTray);

  // Note: callback would be invoked by user interaction in a real scenario
  // In test environment, we verify it's set correctly
  EXPECT_NE(testTray.notification_cb, nullptr);

  // Clear notification
  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  testTray.notification_cb = nullptr;
  tray_update(&testTray);
  waitForNativeNotificationTimeout();
}

TEST_F(TrayTest, TestTooltipUpdate) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test initial tooltip
  EXPECT_STREQ(testTray.tooltip, "TestTray");

  // Update tooltip
  testTray.tooltip = "Updated Tooltip Text";
  tray_update(&testTray);
  EXPECT_STREQ(testTray.tooltip, "Updated Tooltip Text");

  // Restore original tooltip
  testTray.tooltip = "TestTray";
  tray_update(&testTray);
}

TEST_F(TrayTest, TestMenuItemContext) {
  static int contextValue = 42;
  static bool contextCallbackInvoked = false;

  auto context_callback = [](struct tray_menu *item) {  // NOSONAR(cpp:S995): must match tray_menu.cb signature void(*)(struct tray_menu*)
    if (item->context != nullptr) {
      const auto *value = static_cast<const int *>(item->context);
      contextCallbackInvoked = (*value == 42);
    }
  };

  // Create menu with context
  std::array<struct tray_menu, 2> context_menu_arr = {{{.text = "Context Item", .cb = context_callback, .context = &contextValue}, {.text = nullptr}}};
  struct tray_menu *context_menu = context_menu_arr.data();

  testTray.menu = context_menu;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify context is set
  EXPECT_EQ(testTray.menu[0].context, &contextValue);

  // Invoke callback with context
  testTray.menu[0].cb(&testTray.menu[0]);
  EXPECT_TRUE(contextCallbackInvoked);

  // Restore original menu
  testTray.menu = submenu;
}

TEST_F(TrayTest, TestCheckboxStates) {
  testTray.icon = TRAY_ICON_SVG;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  EXPECT_EQ(testTray.menu[1].checkbox, 1);
  EXPECT_EQ(testTray.menu[1].checked, 1);

  // Show menu open with checkbox in checked state
  captureMenuStateAndExit("tray_menu_checkbox_checked");

  // Re-initialize tray with checkbox unchecked
  trayRunning = false;
  testTray.menu[1].checked = 0;
  initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Show menu open with checkbox in unchecked state
  captureMenuStateAndExit("tray_menu_checkbox_unchecked");

  // Restore initial checked state
  testTray.menu[1].checked = 1;
}

TEST_P(TrayIconTest, TestMultipleIconUpdates) {
  const auto &iconParam = GetParam();
  testTray.icon = iconParam.icon;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Update icon multiple times
  testTray.icon = iconParam.alternateIcon;
  tray_update(&testTray);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot(std::string("tray_icon_update_") + iconParam.name));

  testTray.icon = iconParam.icon;
  tray_update(&testTray);
}

TEST_F(TrayTest, TestCompleteMenuHierarchy) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify complete menu structure
  int menuCount = 0;
  for (const struct tray_menu *m = testTray.menu; m->text != nullptr; m++) {
    menuCount++;
  }
  EXPECT_EQ(menuCount, 7);  // Hello, Checked, Disabled, Sep, SubMenu, Sep, Quit

  // Verify all nested submenus
  ASSERT_NE(testTray.menu[4].submenu, nullptr);
  ASSERT_NE(testTray.menu[4].submenu[0].submenu, nullptr);
  ASSERT_NE(testTray.menu[4].submenu[1].submenu, nullptr);
}

TEST_F(TrayTest, TestIconPathArray) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  EXPECT_EQ(testTray.icon, TRAY_ICON1);

  testTray.icon = TRAY_ICON2;
  tray_update(&testTray);
}

TEST_F(TrayTest, TestQuitCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify quit callback exists
  ASSERT_NE(testTray.menu[6].cb, nullptr);
  EXPECT_STREQ(testTray.menu[6].text, "Quit");

  // Note: Actually calling quit_cb would terminate the tray,
  // which is tested separately in TestTrayExit
}

TEST_F(TrayTest, TestTrayShowMenu) {
  testTray.icon = TRAY_ICON_SVG;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Screenshot shows the full menu open, including the SubMenu entry that leads to nested items
  captureMenuStateAndExit("tray_menu_shown");
}

TEST_F(TrayTest, TestTrayExit) {
  tray_exit();
}

TEST_F(TrayTest, TestMenuAppearsOnLeftClick) {
  // Regression test for: clicking the tray icon did not bring up the menu.
  // The activated(Trigger) signal was not connected to the menu popup logic.
  // tray_show_menu() exercises the same code path that the activated handler calls.
  testTray.icon = TRAY_ICON_SVG;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  captureMenuStateAndExit("tray_menu_left_click");
}

TEST_P(TrayNotificationIconTest, TestNotificationCallbackFiredOnClick) {
  // Regression test for: clicking a notification did not invoke the callback.
  // The test hook exercises the same stored callback used by Qt's messageClicked signal.
  const auto &iconParam = GetParam();
  testTray.icon = iconParam.icon;
  static bool callbackInvoked = false;
  callbackInvoked = false;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  testTray.notification_title = "Clickable Notification";
  testTray.notification_text = "Click to test callback";
  testTray.notification_icon = iconParam.icon;
  testTray.notification_cb = []() {
    callbackInvoked = true;
  };
  tray_update(&testTray);

  // Allow the notification to be sent before simulating the click.
  WaitForTrayReady();

  tray_simulate_notification_click();
  tray_loop(0);

  EXPECT_TRUE(callbackInvoked);

  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  testTray.notification_cb = nullptr;
  tray_update(&testTray);
  waitForNativeNotificationTimeout();
}

TEST_F(TrayTest, TestMenuCallbackAfterNotificationUpdate) {
  static int callbackCount = 0;
  callbackCount = 0;

  auto first_item_callback = [](struct tray_menu *) {
    callbackCount++;
  };

  void (*original_cb)(struct tray_menu *) = testTray.menu[0].cb;
  testTray.menu[0].cb = first_item_callback;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  tray_simulate_menu_item_click(0);
  tray_loop(0);
  EXPECT_EQ(callbackCount, 1);

  testTray.notification_title = "Menu Callback Regression";
  testTray.notification_text = "Notification update should not break menu callbacks";
  testTray.notification_icon = TRAY_ICON_SVG;
  tray_update(&testTray);
  WaitForTrayReady();

  tray_simulate_menu_item_click(0);
  tray_loop(0);
  EXPECT_EQ(callbackCount, 2);

  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  tray_update(&testTray);
  waitForNativeNotificationTimeout();

  testTray.menu[0].cb = original_cb;
}

INSTANTIATE_TEST_SUITE_P(
  TrayIcons,
  TrayIconTest,
  ::testing::ValuesIn(TRAY_ICON_PARAMS),
  trayIconParamName
);

INSTANTIATE_TEST_SUITE_P(
  TrayNotificationIcons,
  TrayNotificationIconTest,
  ::testing::ValuesIn(TRAY_ICON_PARAMS),
  trayIconParamName
);
