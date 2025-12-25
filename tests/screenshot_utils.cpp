// test includes
#include "screenshot_utils.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <Windows.h>
// clang-format off
  // build fails if PropIdl.h and gdiplus.h are included before Windows.h
  #include <PropIdl.h>
  #include <gdiplus.h>
// clang-format on
#endif

namespace {
  std::filesystem::path g_outputRoot;

  std::string quote_shell_path(const std::filesystem::path &path) {
    std::string input = path.string();
    std::string output;
    output.reserve(input.size() + 2);
    output.push_back('"');
    for (char ch : input) {
      if (ch == '"') {
        output.append("\\\"");
      } else {
        output.push_back(ch);
      }
    }
    output.push_back('"');
    return output;
  }

#ifdef _WIN32
  std::once_flag gdiplusInitFlag;
  bool gdiplusReady = false;
  ULONG_PTR gdiplusToken = 0;
  std::once_flag dpiFlag;
  bool dpiAware = false;

  bool ensure_gdiplus() {
    std::call_once(gdiplusInitFlag, []() {
      Gdiplus::GdiplusStartupInput input;
      gdiplusReady = Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) == Gdiplus::Ok;
    });
    return gdiplusReady;
  }

  bool ensure_dpi_awareness() {
    std::call_once(dpiFlag, []() {
      auto setDPIAware = reinterpret_cast<BOOL(WINAPI *)()>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware"));
      dpiAware = setDPIAware == nullptr || setDPIAware() == TRUE;
    });
    return dpiAware;
  }

  bool png_encoder_clsid(CLSID *clsid) {
    UINT num = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) {
      return false;
    }
    std::vector<BYTE> buffer(size);
    auto info = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    if (Gdiplus::GetImageEncoders(num, size, info) != Gdiplus::Ok) {
      return false;
    }
    for (UINT i = 0; i < num; ++i) {
      if (wcscmp(info[i].MimeType, L"image/png") == 0) {
        *clsid = info[i].Clsid;
        return true;
      }
    }
    return false;
  }

#endif
}  // namespace

namespace screenshot {

  void initialize(const std::filesystem::path &rootDir) {
    g_outputRoot = rootDir / "screenshots";
    std::error_code ec;
    std::filesystem::create_directories(g_outputRoot, ec);
  }

  std::filesystem::path output_root() {
    return g_outputRoot;
  }

#ifdef __APPLE__
  static bool capture_macos(const std::filesystem::path &file, const Options &) {
    std::string cmd = "screencapture -x " + quote_shell_path(file);
    return std::system(cmd.c_str()) == 0;
  }
#endif

#ifdef __linux__
  static bool capture_linux(const std::filesystem::path &file, const Options &) {
    std::string target = quote_shell_path(file);
    if (std::system("which import > /dev/null 2>&1") == 0) {
      std::string cmd = "import -window root " + target;
      if (std::system(cmd.c_str()) == 0) {
        return true;
      }
    }
    std::string cmd = "gnome-screenshot -f " + target;
    return std::system(cmd.c_str()) == 0;
  }
#endif

#ifdef _WIN32
  static bool capture_windows(const std::filesystem::path &file, const Options &) {
    if (!ensure_dpi_awareness()) {
      std::cerr << "Failed to enable DPI awareness" << std::endl;
      return false;
    }
    if (!ensure_gdiplus()) {
      std::cerr << "GDI+ initialization failed" << std::endl;
      return false;
    }

    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND desktop = GetDesktopWindow();
    if ((width <= 0 || height <= 0) && desktop != nullptr) {
      RECT rect {};
      if (GetWindowRect(desktop, &rect)) {
        left = rect.left;
        top = rect.top;
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
      }
    }
    if (width <= 0 || height <= 0) {
      std::cerr << "Desktop dimensions invalid" << std::endl;
      return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (hdcScreen == nullptr) {
      std::cerr << "GetDC(nullptr) failed" << std::endl;
      return false;
    }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (hdcMem == nullptr) {
      std::cerr << "CreateCompatibleDC failed" << std::endl;
      ReleaseDC(nullptr, hdcScreen);
      return false;
    }
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    if (hbm == nullptr) {
      std::cerr << "CreateCompatibleBitmap failed" << std::endl;
      DeleteDC(hdcMem);
      ReleaseDC(nullptr, hdcScreen);
      return false;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BOOL ok = BitBlt(hdcMem, 0, 0, width, height, hdcScreen, left, top, SRCCOPY | CAPTUREBLT);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
      std::cerr << "BitBlt failed with error " << GetLastError() << std::endl;
      DeleteObject(hbm);
      return false;
    }

    Gdiplus::Bitmap bitmap(hbm, nullptr);
    DeleteObject(hbm);

    CLSID pngClsid;
    if (!png_encoder_clsid(&pngClsid)) {
      std::cerr << "PNG encoder CLSID not found" << std::endl;
      return false;
    }
    std::wstring widePath = file.wstring();
    if (bitmap.Save(widePath.c_str(), &pngClsid, nullptr) != Gdiplus::Ok) {
      std::cerr << "GDI+ failed to write " << file << std::endl;
      return false;
    }
    return true;
  }
#endif

  bool is_available(std::string *reason) {
#ifdef __APPLE__
    return true;
#elif defined(__linux__)
    if (std::system("which import > /dev/null 2>&1") == 0 || std::system("which gnome-screenshot > /dev/null 2>&1") == 0) {
      return true;
    }
    if (reason) {
      *reason = "Neither ImageMagick 'import' nor gnome-screenshot found";
    }
    return false;
#elif defined(_WIN32)
    if (ensure_gdiplus()) {
      return true;
    }
    if (reason) {
      *reason = "Failed to initialize GDI+";
    }
    return false;
#else
    if (reason) {
      *reason = "Unsupported platform";
    }
    return false;
#endif
  }

  bool capture(const std::string &name, const Options &options) {
    // Add a delay to allow UI elements to render before capturing
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (g_outputRoot.empty()) {
      return false;
    }
    auto file = g_outputRoot / (name + ".png");

#ifdef __APPLE__
    return capture_macos(file, options);
#elif defined(__linux__)
    return capture_linux(file, options);
#elif defined(_WIN32)
    return capture_windows(file, options);
#else
    return false;
#endif
  }

}  // namespace screenshot
