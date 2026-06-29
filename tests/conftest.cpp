// standard includes
#include <filesystem>
#include <mutex>

// lib includes
#define LIZARDBYTE_COMMON_TESTING_KEEP_GTEST_TEST
#define LIZARDBYTE_COMMON_TESTING_NO_GLOBAL_ALIASES
#include <lizardbyte/common/testing.h>

// test includes
#include "tests/screenshot_utils.h"
// Undefine the original TEST macro
#undef TEST

// Redefine TEST to use our BaseTest class, to automatically use our BaseTest fixture
#define TEST(test_case_name, test_name) \
  GTEST_TEST_(test_case_name, test_name, ::BaseTest, ::testing::internal::GetTypeId<::BaseTest>())

/**
 * @brief Base class for tests.
 *
 * This class provides a base test fixture for all tests and adds tray-specific helpers.
 */
class BaseTest: public ::lizardbyte::common::testing::BaseTest {
protected:
  BaseTest() = default;

  ~BaseTest() override = default;

  void SetUp() override {
    ::lizardbyte::common::testing::BaseTest::SetUp();

    // todo: only run this one time, instead of every time a test is run
    // see: https://stackoverflow.com/questions/2435277/googletest-accessing-the-environment-from-a-test
    // get command line args from the test executable
    testArgs_ = getArgs();

    // then get the directory of the test executable
    // std::string path = ::testing::internal::GetArgvs()[0];
    testBinary_ = testArgs_[0];

    // get the directory of the test executable
    testBinaryDir_ = std::filesystem::path(testBinary_).parent_path();

    // If testBinaryDir is empty or `.` then set it to the current directory
    // maybe some better options here: https://stackoverflow.com/questions/875249/how-to-get-current-directory
    if (testBinaryDir_.empty() || testBinaryDir_.string() == ".") {
      testBinaryDir_ = std::filesystem::current_path();
    }

    initializeScreenshotsOnce();
  }

  bool ensureScreenshotReady() {
    if (screenshotsReady_) {
      return true;
    }
    if (std::string reason; !screenshot::is_available(&reason)) {
      screenshotUnavailableReason_ = reason;
      return false;
    }
    if (const auto root = screenshot::output_root(); root.empty()) {
      screenshotUnavailableReason_ = "Screenshot output directory not initialized";
      return false;
    }
    screenshotsReady_ = true;
    return true;
  }

  bool captureScreenshot(const std::string &name) const {
    if (!screenshotsReady_) {
      return false;
    }
    bool ok = screenshot::capture(name);
    if (!ok) {
      std::cout << "Failed to capture screenshot: " << name << std::endl;
    }
    return ok;
  }

  std::filesystem::path screenshotsRoot() const {
    return screenshot::output_root();
  }

  [[nodiscard]] const std::filesystem::path &testBinaryDir() const {
    return testBinaryDir_;
  }

  [[nodiscard]] const std::string &screenshotUnavailableReason() const {
    return screenshotUnavailableReason_;
  }

private:
  void initializeScreenshotsOnce() const {
    static std::once_flag screenshotInitFlag;
    std::call_once(screenshotInitFlag, [this]() {
      auto root = testBinaryDir_;
      if (!root.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(root / "screenshots", ec);
      }
      screenshot::initialize(root);
    });
  }

  std::vector<std::string> testArgs_;  // CLI arguments used
  std::filesystem::path testBinary_;  // full path of this binary
  std::filesystem::path testBinaryDir_;  // full directory of this binary
  bool screenshotsReady_ {false};
  std::string screenshotUnavailableReason_;
};

using LinuxTest = ::lizardbyte::common::testing::LinuxTest;
using MacOSTest = ::lizardbyte::common::testing::MacOSTest;
using WindowsTest = ::lizardbyte::common::testing::WindowsTest;
