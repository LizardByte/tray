// test includes
#include "tests/conftest.cpp"

// standard includes
#include <chrono>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
  #include <Windows.h>
// clang-format off
  // build fails if shellapi.h is included before Windows.h
  #include <shellapi.h>
  // clang-format on
  #define TRAY_WINAPI 1
#elif defined(__linux__) || defined(linux) || defined(__linux)
  #define TRAY_APPINDICATOR 1
#elif defined(__APPLE__) || defined(__MACH__)
  #include <Carbon/Carbon.h>
  #define TRAY_APPKIT 1
#endif

// local includes
#include "src/tray.h"
#include "tests/screenshot_utils.h"

#if TRAY_APPINDICATOR
  #define TRAY_ICON1 "mail-message-new"
  #define TRAY_ICON2 "mail-message-new"
#elif TRAY_APPKIT
  #define TRAY_ICON1 "icon.png"
  #define TRAY_ICON2 "icon.png"
#elif TRAY_WINAPI
  #define TRAY_ICON1 "icon.ico"
  #define TRAY_ICON2 "icon.ico"
#endif

class TrayTest: public BaseTest {
protected:
  static struct tray testTray;
  bool trayRunning;
  std::string iconPath1;
  std::string iconPath2;

  // Static arrays for submenus
  static struct tray_menu submenu7_8[];
  static struct tray_menu submenu5_6[];
  static struct tray_menu submenu_second[];
  static struct tray_menu submenu[];

  // Non-static member functions
  static void hello_cb(struct tray_menu *item) {
    // Mock implementation
  }

  static void toggle_cb(struct tray_menu *item) {
    item->checked = !item->checked;
    tray_update(&testTray);
  }

  static void quit_cb(struct tray_menu *item) {
    tray_exit();
  }

  static void submenu_cb(struct tray_menu *item) {
    // Mock implementation
    tray_update(&testTray);
  }

  void SetUp() override {
    BaseTest::SetUp();

    // Skip tests if screenshot tooling is not available
    if (!ensureScreenshotReady()) {
      GTEST_SKIP() << "Screenshot tooling missing: " << screenshotUnavailableReason;
    }
    if (screenshot::output_root().empty()) {
      GTEST_SKIP() << "Screenshot output path not initialized";
    }

#if defined(TRAY_WINAPI) || defined(TRAY_APPKIT)
    // Ensure icon files exist in test binary directory
    // Look for icons in project root or cmake build directory
    std::filesystem::path projectRoot = testBinaryDir.parent_path();
    std::filesystem::path iconSource;

    // Try icons directory first
    if (std::filesystem::exists(projectRoot / "icons" / TRAY_ICON1)) {
      iconSource = projectRoot / "icons" / TRAY_ICON1;
    }
    // Try project root
    else if (std::filesystem::exists(projectRoot / TRAY_ICON1)) {
      iconSource = projectRoot / TRAY_ICON1;
    }
    // Try current directory
    else if (std::filesystem::exists(std::filesystem::path(TRAY_ICON1))) {
      iconSource = std::filesystem::path(TRAY_ICON1);
    }

    // Copy icon to test binary directory if not already there
    if (!iconSource.empty()) {
      std::filesystem::path iconDest = testBinaryDir / TRAY_ICON1;
      if (!std::filesystem::exists(iconDest)) {
        std::error_code ec;
        std::filesystem::copy_file(iconSource, iconDest, ec);
        if (ec) {
          std::cout << "Warning: Failed to copy icon file: " << ec.message() << std::endl;
        }
      }
    }
#endif

    trayRunning = false;
    testTray.icon = TRAY_ICON1;
    testTray.tooltip = "TestTray";
    testTray.menu = submenu;
    submenu[1].checked = 1;  // Reset checkbox state to initial value
  }

  void TearDown() override {
    ShutdownTray();
    BaseTest::TearDown();
  }

  void ShutdownTray() {
    if (!trayRunning) {
      return;
    }
    tray_exit();
    tray_loop(0);
    trayRunning = false;
  }

  // Process pending GTK events to allow AppIndicator to register
  // Call this ONLY before screenshots to ensure the icon is visible
  void WaitForTrayReady() {
#if defined(TRAY_APPINDICATOR)
    // On Linux with AppIndicator, process GTK events to allow D-Bus registration
    // This ensures the icon actually appears in the system tray before screenshots
    // Use shorter iterations to avoid interfering with event loop state
    for (int i = 0; i < 100; i++) {
      tray_loop(0);  // Non-blocking - process pending events
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
  }
};

// Define the static arrays
struct tray_menu TrayTest::submenu7_8[] = {
  {.text = "7", .cb = submenu_cb},
  {.text = "-"},
  {.text = "8", .cb = submenu_cb},
  {.text = nullptr}
};
struct tray_menu TrayTest::submenu5_6[] = {
  {.text = "5", .cb = submenu_cb},
  {.text = "6", .cb = submenu_cb},
  {.text = nullptr}
};
struct tray_menu TrayTest::submenu_second[] = {
  {.text = "THIRD", .submenu = submenu7_8},
  {.text = "FOUR", .submenu = submenu5_6},
  {.text = nullptr}
};
struct tray_menu TrayTest::submenu[] = {
  {.text = "Hello", .cb = hello_cb},
  {.text = "Checked", .checked = 1, .checkbox = 1, .cb = toggle_cb},
  {.text = "Disabled", .disabled = 1},
  {.text = "-"},
  {.text = "SubMenu", .submenu = submenu_second},
  {.text = "-"},
  {.text = "Quit", .cb = quit_cb},
  {.text = nullptr}
};
struct tray TrayTest::testTray = {
  .icon = TRAY_ICON1,
  .tooltip = "TestTray",
  .menu = submenu
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
  int result = tray_loop(1);
  EXPECT_EQ(result, 0);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_loop_iteration"));
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
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_updated"));

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
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_menu_toggle"));
}

TEST_F(TrayTest, TestMenuItemCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test hello callback - it should work without crashing
  ASSERT_NE(testTray.menu[0].cb, nullptr);
  testTray.menu[0].cb(&testTray.menu[0]);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_menu_callback_hello"));
}

TEST_F(TrayTest, TestDisabledMenuItem) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify disabled menu item
  EXPECT_EQ(testTray.menu[2].disabled, 1);
  EXPECT_STREQ(testTray.menu[2].text, "Disabled");
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_menu_disabled_item"));
}

TEST_F(TrayTest, TestMenuSeparator) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify separator exists
  EXPECT_STREQ(testTray.menu[3].text, "-");
  EXPECT_EQ(testTray.menu[3].cb, nullptr);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_menu_with_separator"));
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

  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_submenu_structure"));
}

TEST_F(TrayTest, TestSubmenuCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test submenu callback
  ASSERT_NE(testTray.menu[4].submenu[0].submenu[0].cb, nullptr);
  testTray.menu[4].submenu[0].submenu[0].cb(&testTray.menu[4].submenu[0].submenu[0]);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_submenu_callback_executed"));
}

TEST_F(TrayTest, TestNotificationDisplay) {
#if !(defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
  GTEST_SKIP() << "Notifications only supported on desktop platforms";
#endif

#if defined(_WIN32)
  QUERY_USER_NOTIFICATION_STATE notification_state;
  HRESULT ns = SHQueryUserNotificationState(&notification_state);
  if (ns != S_OK || notification_state != QUNS_ACCEPTS_NOTIFICATIONS) {
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
  tray_loop(1);

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
  HRESULT ns = SHQueryUserNotificationState(&notification_state);
  if (ns != S_OK || notification_state != QUNS_ACCEPTS_NOTIFICATIONS) {
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
  tray_loop(1);

  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_notification_with_callback"));

  // Note: callback would be invoked by user interaction in real scenario
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
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_tooltip_initial"));

  // Update tooltip
  testTray.tooltip = "Updated Tooltip Text";
  tray_update(&testTray);
  EXPECT_STREQ(testTray.tooltip, "Updated Tooltip Text");
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_tooltip_updated"));

  // Restore original tooltip
  testTray.tooltip = "TestTray";
  tray_update(&testTray);
}

TEST_F(TrayTest, TestMenuItemContext) {
  static int contextValue = 42;
  static bool contextCallbackInvoked = false;

  auto context_callback = [](struct tray_menu *item) {
    if (item->context != nullptr) {
      int *value = static_cast<int *>(item->context);
      contextCallbackInvoked = (*value == 42);
    }
  };

  // Create menu with context
  struct tray_menu context_menu[] = {
    {.text = "Context Item", .cb = context_callback, .context = &contextValue},
    {.text = nullptr}
  };

  testTray.menu = context_menu;

  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify context is set
  EXPECT_EQ(testTray.menu[0].context, &contextValue);

  // Invoke callback with context
  testTray.menu[0].cb(&testTray.menu[0]);
  EXPECT_TRUE(contextCallbackInvoked);

  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_menu_with_context"));

  // Restore original menu
  testTray.menu = submenu;
}

TEST_F(TrayTest, TestCheckboxStates) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Test checkbox item
  EXPECT_EQ(testTray.menu[1].checkbox, 1);
  EXPECT_EQ(testTray.menu[1].checked, 1);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_checkbox_checked"));

  // Toggle checkbox
  testTray.menu[1].checked = 0;
  tray_update(&testTray);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_checkbox_unchecked"));

  // Toggle back
  testTray.menu[1].checked = 1;
  tray_update(&testTray);
}

TEST_F(TrayTest, TestMultipleIconUpdates) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Capture initial icon
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_state1"));

  // Update icon multiple times
  testTray.icon = TRAY_ICON2;
  tray_update(&testTray);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_state2"));

  testTray.icon = TRAY_ICON1;
  tray_update(&testTray);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_state3"));
}

TEST_F(TrayTest, TestCompleteMenuHierarchy) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify complete menu structure
  int menuCount = 0;
  for (struct tray_menu *m = testTray.menu; m->text != nullptr; m++) {
    menuCount++;
  }
  EXPECT_EQ(menuCount, 7);  // Hello, Checked, Disabled, Sep, SubMenu, Sep, Quit

  // Verify all nested submenus
  ASSERT_NE(testTray.menu[4].submenu, nullptr);
  ASSERT_NE(testTray.menu[4].submenu[0].submenu, nullptr);
  ASSERT_NE(testTray.menu[4].submenu[1].submenu, nullptr);

  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_complete_menu_hierarchy"));
}

TEST_F(TrayTest, TestIconPathArray) {
#if defined(TRAY_WINAPI)
  // Test icon path array caching (Windows-specific feature)
  // Allocate memory for tray struct with flexible array member
  const size_t icon_count = 2;
  struct tray *iconCacheTray = (struct tray *) malloc(
    sizeof(struct tray) + icon_count * sizeof(const char *)
  );
  ASSERT_NE(iconCacheTray, nullptr);

  // Initialize the tray structure
  iconCacheTray->icon = TRAY_ICON1;
  iconCacheTray->tooltip = "Icon Cache Test";
  iconCacheTray->notification_icon = nullptr;
  iconCacheTray->notification_text = nullptr;
  iconCacheTray->notification_title = nullptr;
  iconCacheTray->notification_cb = nullptr;
  iconCacheTray->menu = submenu;
  *const_cast<int *>(&iconCacheTray->iconPathCount) = icon_count;
  *const_cast<const char **>(&iconCacheTray->allIconPaths[0]) = TRAY_ICON1;
  *const_cast<const char **>(&iconCacheTray->allIconPaths[1]) = TRAY_ICON2;

  int initResult = tray_init(iconCacheTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify initial icon
  EXPECT_EQ(iconCacheTray->icon, TRAY_ICON1);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_cache_initial"));

  // Switch to cached icon
  iconCacheTray->icon = TRAY_ICON2;
  tray_update(iconCacheTray);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_cache_updated"));
  free(iconCacheTray);
#else
  // On non-Windows platforms, just test basic icon switching
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  EXPECT_EQ(testTray.icon, TRAY_ICON1);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_cache_initial"));

  testTray.icon = TRAY_ICON2;
  tray_update(&testTray);
  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_icon_cache_updated"));
#endif
}

TEST_F(TrayTest, TestQuitCallback) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Verify quit callback exists
  ASSERT_NE(testTray.menu[6].cb, nullptr);
  EXPECT_STREQ(testTray.menu[6].text, "Quit");

  tray_loop(1);
  WaitForTrayReady();
  EXPECT_TRUE(captureScreenshot("tray_before_quit"));

  // Note: Actually calling quit_cb would terminate the tray,
  // which is tested separately in TestTrayExit
}

TEST_F(TrayTest, TestTrayShowMenu) {
  int initResult = tray_init(&testTray);
  trayRunning = (initResult == 0);
  ASSERT_EQ(initResult, 0);

  // Start a thread to capture screenshot and exit
  std::thread capture_thread([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    WaitForTrayReady();
    EXPECT_TRUE(captureScreenshot("tray_menu_shown"));
#if defined(TRAY_WINAPI)
    // Cancel the menu
    PostMessage(tray_get_hwnd(), WM_CANCELMODE, 0, 0);
    // Wait for menu to close
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#elif defined(TRAY_APPKIT)
    // Simulate ESC key to dismiss menu
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, kVK_Escape, true);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    CGEventRef event2 = CGEventCreateKeyboardEvent(NULL, kVK_Escape, false);
    CGEventPost(kCGHIDEventTap, event2);
    CFRelease(event2);
    // Wait for menu to close
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    // Exit the tray
    tray_exit();
  });

  // Show the menu programmatically
  tray_show_menu();

  tray_loop(1);

  capture_thread.join();
}

TEST_F(TrayTest, TestTrayExit) {
  tray_exit();
}
