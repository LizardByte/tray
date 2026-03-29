// test includes
#include "tests/conftest.cpp"

// standard includes
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
  #include <Windows.h>
// clang-format off
  // build fails if shellapi.h is included before Windows.h
  #include <shellapi.h>
  // clang-format on
  #define TRAY_WINAPI 1
#elif defined(__linux__) || defined(linux) || defined(__linux)
  #define TRAY_QT 1
#elif defined(__APPLE__) || defined(__MACH__)
  #include <Carbon/Carbon.h>
  #define TRAY_APPKIT 1
#endif

// local includes
#include "src/tray.h"
#include "tests/screenshot_utils.h"

#if TRAY_QT
constexpr const char *TRAY_ICON1 = "icon.png";
constexpr const char *TRAY_ICON2 = "icon.png";
constexpr const char *TRAY_ICON_SVG = "icon.svg";
constexpr const char *TRAY_ICON_THEMED = "mail-message-new";
#elif TRAY_APPKIT
constexpr const char *TRAY_ICON1 = "icon.png";
constexpr const char *TRAY_ICON2 = "icon.png";
#elif TRAY_WINAPI
constexpr const char *TRAY_ICON1 = "icon.ico";
constexpr const char *TRAY_ICON2 = "icon.ico";
#endif

// File-scope tray data shared across all TrayTest instances
namespace {
  struct tray_menu g_submenu7_8[] = {  // NOSONAR(cpp:S5945, cpp:S5421) - C-style array with null sentinel required by tray C API; mutable for runtime callback assignment
    {.text = "7", .cb = nullptr},
    {.text = "-"},
    {.text = "8", .cb = nullptr},
    {.text = nullptr}
  };
  struct tray_menu g_submenu5_6[] = {  // NOSONAR(cpp:S5945, cpp:S5421) - C-style array with null sentinel required by tray C API; mutable for runtime callback assignment
    {.text = "5", .cb = nullptr},
    {.text = "6", .cb = nullptr},
    {.text = nullptr}
  };
  struct tray_menu g_submenu_second[] = {  // NOSONAR(cpp:S5945, cpp:S5421) - C-style array with null sentinel required by tray C API; mutable for runtime callback assignment
    {.text = "THIRD", .submenu = g_submenu7_8},
    {.text = "FOUR", .submenu = g_submenu5_6},
    {.text = nullptr}
  };
  struct tray_menu g_submenu[] = {  // NOSONAR(cpp:S5945, cpp:S5421) - C-style array with null sentinel required by tray C API; mutable for runtime callback assignment
    {.text = "Hello", .cb = nullptr},
    {.text = "Checked", .checked = 1, .checkbox = 1, .cb = nullptr},
    {.text = "Disabled", .disabled = 1},
    {.text = "-"},
    {.text = "SubMenu", .submenu = g_submenu_second},
    {.text = "-"},
    {.text = "Quit", .cb = nullptr},
    {.text = nullptr}
  };
  struct tray g_testTray = {  // NOSONAR(cpp:S5421) - mutable global required for shared tray state across TEST_F instances
    .icon = TRAY_ICON1,
    .tooltip = "TestTray",
    .menu = g_submenu
  };
}  // namespace

class TrayTest: public BaseTest {  // NOSONAR(cpp:S3656) - fixture members must be protected for TEST_F-generated subclasses
protected:  // NOSONAR(cpp:S3656) - TEST_F generates subclasses that need access to fixture state/methods
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

  // Dismisses the open menu and exits the tray event loop from a background thread.
  void closeMenuAndExit() {
#if defined(TRAY_WINAPI)
    PostMessage(tray_get_hwnd(), WM_CANCELMODE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#elif defined(TRAY_APPKIT)
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, kVK_Escape, true);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    CGEventRef event2 = CGEventCreateKeyboardEvent(NULL, kVK_Escape, false);
    CGEventPost(kCGHIDEventTap, event2);
    CFRelease(event2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    tray_exit();
  }

  // Capture a screenshot while the tray menu is open, then dismiss and exit.
  void captureMenuStateAndExit(const char *screenshotName) {
    std::thread capture_thread([this, screenshotName]() {  // NOSONAR(cpp:S6168) - std::jthread is unavailable on AppleClang 17/libc++ used in CI
      EXPECT_TRUE(captureScreenshot(screenshotName));
      closeMenuAndExit();
    });

    tray_show_menu();
    while (tray_loop(0) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    capture_thread.join();
  }

  bool trayRunning {false};  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern
  struct tray &testTray = g_testTray;  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern
  struct tray_menu *submenu = g_submenu;  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern
  struct tray_menu *submenu7_8 = g_submenu7_8;  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern
  struct tray_menu *submenu5_6 = g_submenu5_6;  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern
  struct tray_menu *submenu_second = g_submenu_second;  // NOSONAR(cpp:S3656) - protected access required by gtest TEST_F subclass pattern

  static void hello_cb([[maybe_unused]] struct tray_menu *item) {
    // Mock implementation
  }

  static void toggle_cb([[maybe_unused]] struct tray_menu *item) {  // NOSONAR(cpp:S1172) - unused param required by tray_menu.cb function pointer type
    g_testTray.menu[1].checked = !g_testTray.menu[1].checked;
    tray_update(&g_testTray);
  }

  static void quit_cb([[maybe_unused]] struct tray_menu *item) {  // NOSONAR(cpp:S1172) - unused param required by tray_menu.cb function pointer type
    tray_exit();
  }

  static void submenu_cb([[maybe_unused]] struct tray_menu *item) {  // NOSONAR(cpp:S1172) - unused param required by tray_menu.cb function pointer type
    // Mock implementation
    tray_update(&g_testTray);
  }

  void SetUp() override {
    BaseTest::SetUp();

    // Wire up callbacks (file-scope arrays can't use addresses of class statics at init time)
    g_submenu[0].cb = hello_cb;
    g_submenu[1].cb = toggle_cb;
    g_submenu[6].cb = quit_cb;
    g_submenu7_8[0].cb = submenu_cb;
    g_submenu7_8[2].cb = submenu_cb;
    g_submenu5_6[0].cb = submenu_cb;
    g_submenu5_6[1].cb = submenu_cb;

    // Skip tests if screenshot tooling is not available
    if (!ensureScreenshotReady()) {
      GTEST_SKIP() << "Screenshot tooling missing: " << screenshotUnavailableReason;
    }
    if (screenshot::output_root().empty()) {
      GTEST_SKIP() << "Screenshot output path not initialized";
    }

    // Ensure icon files exist in test binary directory
    std::filesystem::path projectRoot = testBinaryDir.parent_path();
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
        std::filesystem::path iconDest = testBinaryDir / iconName;
        if (!std::filesystem::exists(iconDest)) {
          std::error_code ec;
          std::filesystem::copy_file(iconSource, iconDest, ec);
          if (ec) {
            std::cout << "Warning: Failed to copy icon file: " << ec.message() << std::endl;
          }
        }
      }
    };

    ensureIconInTestDir(TRAY_ICON1);
#if defined(TRAY_QT)
    ensureIconInTestDir(TRAY_ICON_SVG);
#endif

    trayRunning = false;
    testTray.icon = TRAY_ICON1;
    testTray.tooltip = "TestTray";
    testTray.notification_title = nullptr;
    testTray.notification_text = nullptr;
    testTray.notification_icon = nullptr;
    testTray.notification_cb = nullptr;
    testTray.menu = g_submenu;
    g_submenu[1].checked = 1;
  }

  void TearDown() override {
    ShutdownTray();
    BaseTest::TearDown();
  }

  // Process pending events to allow tray icon to appear.
  // Call this ONLY before screenshots to ensure the icon is visible.
  void WaitForTrayReady() {
#if defined(TRAY_QT)
    for (int i = 0; i < 100; i++) {
      tray_loop(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#elif defined(TRAY_APPKIT)
    static std::thread::id main_thread_id = std::this_thread::get_id();
    if (std::this_thread::get_id() == main_thread_id) {
      for (int i = 0; i < 100; i++) {
        tray_loop(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
#endif
  }
};

TEST_F(TrayTest, TestTrayInit) {
  int result = tray_init(&testTray);
  trayRunning = (result == 0);
  EXPECT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_initial"));
}

TEST_F(TrayTest, TestTrayLoop) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);
  // Test non-blocking loop (blocking=0) since blocking would hang without events
  int result = tray_loop(0);
  EXPECT_EQ(result, 0);
}

#if defined(TRAY_WINAPI)
TEST_F(TrayTest, TestTrayLoopHandlesThreadQuitMessage) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // WM_QUIT is posted to the thread queue, not to a specific window.
  PostQuitMessage(0);

  bool sawQuit = false;
  for (int i = 0; i < 200; ++i) {
    if (tray_loop(0) == -1) {
      sawQuit = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(sawQuit);
}
#endif

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

TEST_F(TrayTest, TestNotificationDisplay) {
#if !(defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
  GTEST_SKIP() << "Notifications only supported on desktop platforms";
#endif

#if defined(_WIN32)
  QUERY_USER_NOTIFICATION_STATE notification_state;
  if (HRESULT ns = SHQueryUserNotificationState(&notification_state);
      ns != S_OK || notification_state != QUNS_ACCEPTS_NOTIFICATIONS) {
    GTEST_SKIP() << "Notifications not accepted in this environment. SHQueryUserNotificationState result: " << ns << ", state: " << notification_state;
  }
#endif

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Set notification properties
  testTray.notification_title = "Test Notification";
  testTray.notification_text = "This is a test notification message";
  testTray.notification_icon = TRAY_ICON1;

  tray_update(&testTray);

  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_notification_displayed"));

  // Clear notification
  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  tray_update(&testTray);
}

TEST_F(TrayTest, TestNotificationCallback) {
#if !(defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
  GTEST_SKIP() << "Notifications only supported on desktop platforms";
#endif

#if defined(_WIN32)
  QUERY_USER_NOTIFICATION_STATE notification_state;
  if (HRESULT ns = SHQueryUserNotificationState(&notification_state);
      ns != S_OK || notification_state != QUNS_ACCEPTS_NOTIFICATIONS) {
    GTEST_SKIP() << "Notifications not accepted in this environment. SHQueryUserNotificationState result: " << ns << ", state: " << notification_state;
  }
#endif

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
  testTray.notification_icon = TRAY_ICON1;
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

  auto context_callback = [](struct tray_menu *item) {  // NOSONAR(cpp:S995) - must match tray_menu.cb signature void(*)(struct tray_menu*)
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
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  EXPECT_EQ(testTray.menu[1].checkbox, 1);
  EXPECT_EQ(testTray.menu[1].checked, 1);

  // Show menu open with checkbox in checked state
  captureMenuStateAndExit("tray_menu_checkbox_checked");  // NOSONAR(cpp:S6168) - helper uses std::thread for AppleClang 17 compatibility

  // Re-initialize tray with checkbox unchecked
  trayRunning = false;
  testTray.menu[1].checked = 0;
  initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Show menu open with checkbox in unchecked state
  captureMenuStateAndExit("tray_menu_checkbox_unchecked");  // NOSONAR(cpp:S6168) - helper uses std::thread for AppleClang 17 compatibility

  // Restore initial checked state
  testTray.menu[1].checked = 1;
}

TEST_F(TrayTest, TestMultipleIconUpdates) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Update icon multiple times
  testTray.icon = TRAY_ICON2;
  tray_update(&testTray);

  testTray.icon = TRAY_ICON1;
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
#if defined(TRAY_WINAPI)
  // Test icon path array caching (Windows-specific feature)
  // The tray struct has a flexible array member, so we allocate a raw buffer
  // and use memcpy to initialize const fields before the object is used.
  const size_t icon_count = 2;
  const size_t buf_size = sizeof(struct tray) + icon_count * sizeof(const char *);
  std::vector<std::byte> buf(buf_size, std::byte {0});
  auto *iconCacheTray = reinterpret_cast<struct tray *>(buf.data());  // NOSONAR(cpp:S3630) - reinterpret_cast required to overlay struct onto raw buffer for flexible array member

  iconCacheTray->icon = TRAY_ICON1;
  iconCacheTray->tooltip = "Icon Cache Test";
  iconCacheTray->notification_icon = nullptr;
  iconCacheTray->notification_text = nullptr;
  iconCacheTray->notification_title = nullptr;
  iconCacheTray->notification_cb = nullptr;
  iconCacheTray->menu = submenu;

  // Write const fields via memcpy — const_cast is required to initialize const members in a C struct flexible array allocation
  auto count_val = static_cast<int>(icon_count);
  std::memcpy(const_cast<int *>(&iconCacheTray->iconPathCount), &count_val, sizeof(count_val));  // NOSONAR(cpp:S859) - required to initialize const member in C struct allocated via raw buffer
  const char *icon1 = TRAY_ICON1;
  const char *icon2 = TRAY_ICON2;
  std::memcpy(const_cast<char **>(&iconCacheTray->allIconPaths[0]), &icon1, sizeof(icon1));  // NOSONAR(cpp:S859) - required to initialize const member in C struct allocated via raw buffer
  std::memcpy(const_cast<char **>(&iconCacheTray->allIconPaths[1]), &icon2, sizeof(icon2));  // NOSONAR(cpp:S859) - required to initialize const member in C struct allocated via raw buffer

  int initResult = tray_init(iconCacheTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify initial icon
  EXPECT_EQ(iconCacheTray->icon, TRAY_ICON1);

  // Switch to cached icon
  iconCacheTray->icon = TRAY_ICON2;
  tray_update(iconCacheTray);
  // buf goes out of scope, no manual free needed
#else
  // On non-Windows platforms, just test basic icon switching
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  EXPECT_EQ(testTray.icon, TRAY_ICON1);

  testTray.icon = TRAY_ICON2;
  tray_update(&testTray);
#endif
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
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Screenshot shows the full menu open, including the SubMenu entry that leads to nested items
  captureMenuStateAndExit("tray_menu_shown");  // NOSONAR(cpp:S6168) - helper uses std::thread for AppleClang 17 compatibility
}

TEST_F(TrayTest, TestTrayExit) {
  tray_exit();
}

#if defined(TRAY_QT)

TEST_F(TrayTest, TestTrayIconThemed) {
  testTray.icon = TRAY_ICON_THEMED;
  int result = tray_init(&testTray);
  trayRunning = (result == 0);
  ASSERT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_themed"));
  testTray.icon = TRAY_ICON1;
}

TEST_F(TrayTest, TestTrayIconSvgFile) {
  testTray.icon = TRAY_ICON_SVG;
  int result = tray_init(&testTray);
  trayRunning = (result == 0);
  ASSERT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_svg"));
  testTray.icon = TRAY_ICON1;
}

TEST_F(TrayTest, TestNotificationWithThemedIcon) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  testTray.notification_title = "Test Notification";
  testTray.notification_text = "This is a test notification message";
  testTray.notification_icon = TRAY_ICON_THEMED;
  tray_update(&testTray);

  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_notification_themed_icon"));

  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  tray_update(&testTray);
}

TEST_F(TrayTest, TestMenuAppearsOnLeftClick) {
  // Regression test for: clicking the tray icon did not bring up the menu.
  // The activated(Trigger) signal was not connected to the menu popup logic.
  // tray_show_menu() exercises the same code path that the activated handler calls.
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  captureMenuStateAndExit("tray_menu_left_click");  // NOSONAR(cpp:S6168) - helper uses std::thread for AppleClang 17 compatibility
}

TEST_F(TrayTest, TestNotificationCallbackFiredOnClick) {
  // Regression test for: clicking a notification did not invoke the callback.
  // On the D-Bus path, QSystemTrayIcon::messageClicked is never emitted; the
  // callback must be routed through TrayNotificationHandler::onActionInvoked.
  static bool callbackInvoked = false;
  callbackInvoked = false;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  testTray.notification_title = "Clickable Notification";
  testTray.notification_text = "Click to test callback";
  testTray.notification_icon = TRAY_ICON1;
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
}

TEST_F(TrayTest, TestMenuCallbackAfterNotificationUpdate) {
  static int callbackCount = 0;
  callbackCount = 0;

  auto first_item_callback = [](struct tray_menu *item) {  // NOSONAR(cpp:S1172) - unused param required by tray_menu.cb function pointer type
    callbackCount++;
    (void) item;
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
  testTray.notification_icon = TRAY_ICON1;
  tray_update(&testTray);
  WaitForTrayReady();

  tray_simulate_menu_item_click(0);
  tray_loop(0);
  EXPECT_EQ(callbackCount, 2);

  testTray.notification_title = nullptr;
  testTray.notification_text = nullptr;
  testTray.notification_icon = nullptr;
  tray_update(&testTray);

  testTray.menu[0].cb = original_cb;
}

#endif  // TRAY_QT
