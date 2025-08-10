/**
 * @file src/tray_windows.c
 * @brief System tray implementation for Windows.
 */
// standard includes
#include <windows.h>
#include <strsafe.h>
// clang-format off
// build fails if shellapi.h is included before windows.h
#include <shellapi.h>
// clang-format on
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
// std C
#include <stdlib.h>
#include <string.h>

// local includes
#include "tray.h"

#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)  ///< Tray callback message.
#define WC_TRAY_CLASS_NAME "TRAY"  ///< Tray window class name.
#define ID_TRAY_FIRST 1000  ///< First tray identifier.

/**
 * @brief Icon information.
 */
struct icon_info {
  const char *path;  ///< Path to the icon
  HICON icon;  ///< Regular icon
  HICON large_icon;  ///< Large icon
  HICON notification_icon;  ///< Notification icon
};

/**
 * @brief Icon type.
 */
enum IconType {
  REGULAR = 1,  ///< Regular icon
  LARGE,  ///< Large icon
  NOTIFICATION  ///< Notification icon
};

static WNDCLASSEXA wc;
static NOTIFYICONDATAA nid;
static HWND hwnd;
static HMENU hmenu = NULL;
static void (*notification_cb)() = 0;
static UINT wm_taskbarcreated;
static struct tray *g_tray = NULL;  // remember last tray so we can re-apply after Explorer restarts
// Stable identity for our notify icon (helps after Explorer restarts and avoids duplicates).
static const GUID kTrayGuid =
  {0xC1A1C4E1, 0x7C42, 0x4DB4, {0x93, 0xB4, 0x2E, 0x9E, 0x0D, 0x7A, 0x8E, 0x31}};

static struct icon_info *icon_infos;
static HMENU _tray_menu(struct tray_menu *m, UINT *id);

// Safe copy that always NUL-terminates
static void safe_copy_sz(char *dst, size_t dstcch, const char *src) {
  if (!dst || dstcch == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  StringCchCopyA(dst, dstcch, src);
}

static int icon_info_count;

static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_TRAY_CALLBACK_MESSAGE: {
      switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
          POINT p;
          GetCursorPos(&p);
          SetForegroundWindow(hwnd);
          WORD cmd = (WORD)TrackPopupMenu(
            hmenu,
            TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            p.x, p.y, 0, hwnd, NULL);
          if (cmd) {
            SendMessage(hwnd, WM_COMMAND, cmd, 0);
          }
          // Ensure the menu dismisses properly (MSDN guidance)
          PostMessage(hwnd, WM_NULL, 0, 0);
          return 0;
        }

        case NIN_BALLOONUSERCLICK:
          if (notification_cb) {
            notification_cb();
          }
          return 0;
      }
      break;
    }
  }

  // Handle Explorer restarts: re-add icon, set version, and re-apply state.
  if (msg == wm_taskbarcreated) {
    nid.uFlags = NIF_MESSAGE | NIF_GUID;
    nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE; // keep callback current
    Shell_NotifyIconA(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconA(NIM_SETVERSION, &nid);
    if (g_tray) {
      // Rebuild menu and re-apply state
      UINT id = ID_TRAY_FIRST;
      HMENU prevmenu = hmenu;
      hmenu = _tray_menu(g_tray->menu, &id);
      if (prevmenu) DestroyMenu(prevmenu);

      extern void tray_update(struct tray *tray);
      tray_update(g_tray);
    }
    return 0;
  }

  return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HMENU _tray_menu(struct tray_menu *m, UINT *id) {
  HMENU hmenu = CreatePopupMenu();
  for (; m != NULL && m->text != NULL; m++, (*id)++) {
    if (strcmp(m->text, "-") == 0) {
      InsertMenuA(hmenu, *id, MF_SEPARATOR, 0, NULL);
    } else {
      MENUITEMINFOA item;
      memset(&item, 0, sizeof(item));
      item.cbSize = sizeof(MENUITEMINFOA);
      item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
      item.fType = 0;
      item.fState = 0;
      if (m->submenu != NULL) {
        item.fMask |= MIIM_SUBMENU;
        item.hSubMenu = _tray_menu(m->submenu, id);
      }
      if (m->disabled) {
        item.fState |= MFS_DISABLED;
      }
      if (m->checked) {
        item.fState |= MFS_CHECKED;
      }
      item.wID = *id;
      item.dwTypeData = (LPSTR) m->text;
      item.dwItemData = (ULONG_PTR) m;

      InsertMenuItemA(hmenu, *id, TRUE, &item);
    }
  }
  return hmenu;
}

/**
 * @brief Create icon information.
 * @param path Path to the icon.
 * @return Icon information.
 */
struct icon_info _create_icon_info(const char *path) {
  struct icon_info info;
  info.path = strdup(path);

  // These must be separate invocations otherwise Windows may opt to only return large or small icons.
  // MSDN does not explicitly state this anywhere, but it has been observed on some machines.
  ExtractIconExA(path, 0, &info.large_icon, NULL, 1);
  ExtractIconExA(path, 0, NULL, &info.icon, 1);

  info.notification_icon = LoadImageA(NULL, path, IMAGE_ICON, GetSystemMetrics(SM_CXICON) * 2, GetSystemMetrics(SM_CYICON) * 2, LR_LOADFROMFILE);
  return info;
}

/**
 * @brief Initialize icon cache.
 * @param paths Paths to the icons.
 * @param count Number of paths.
 */
void _init_icon_cache(const char **paths, int count) {
  icon_info_count = count;
  icon_infos = malloc(sizeof(struct icon_info) * icon_info_count);

  for (int i = 0; i < count; ++i) {
    icon_infos[i] = _create_icon_info(paths[i]);
  }
}

/**
 * @brief Destroy icon cache.
 */
void _destroy_icon_cache() {
  for (int i = 0; i < icon_info_count; ++i) {
    if (icon_infos[i].icon) DestroyIcon(icon_infos[i].icon);
    if (icon_infos[i].large_icon) DestroyIcon(icon_infos[i].large_icon);
    if (icon_infos[i].notification_icon) DestroyIcon(icon_infos[i].notification_icon);
    free((void *) icon_infos[i].path);
  }

  free(icon_infos);
  icon_infos = NULL;
  icon_info_count = 0;
}

/**
 * @brief Fetch cached icon.
 * @param icon_record Icon record.
 * @param icon_type Icon type.
 * @return Icon.
 */
HICON _fetch_cached_icon(struct icon_info *icon_record, enum IconType icon_type) {
  switch (icon_type) {
    case REGULAR:
      return icon_record->icon;
    case LARGE:
      return icon_record->large_icon;
    case NOTIFICATION:
      return icon_record->notification_icon;
  }
}

/**
 * @brief Fetch icon.
 * @param path Path to the icon.
 * @param icon_type Icon type.
 * @return Icon.
 */
HICON _fetch_icon(const char *path, enum IconType icon_type) {
  // Find a cached icon by path
  for (int i = 0; i < icon_info_count; ++i) {
    if (strcmp(icon_infos[i].path, path) == 0) {
      return _fetch_cached_icon(&icon_infos[i], icon_type);
    }
  }

  // Expand cache, fetch, and retry
  icon_info_count += 1;
  icon_infos = realloc(icon_infos, sizeof(struct icon_info) * icon_info_count);
  icon_infos[icon_info_count - 1] = _create_icon_info(path);

  return _fetch_cached_icon(&icon_infos[icon_info_count - 1], icon_type);
}

int tray_init(struct tray *tray) {
  wm_taskbarcreated = RegisterWindowMessageA("TaskbarCreated");

  _init_icon_cache(tray->allIconPaths, tray->iconPathCount);

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(WNDCLASSEXA);
  wc.lpfnWndProc = _tray_wnd_proc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = WC_TRAY_CLASS_NAME;
  if (!RegisterClassExA(&wc)) {
    return -1;
  }

  // Hidden top-level window (NOT message-only) is safest for Shell_NotifyIcon callbacks.
  hwnd = CreateWindowExA(0, WC_TRAY_CLASS_NAME, NULL, 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);
  if (hwnd == NULL) {
    UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandle(NULL));
    return -1;
  }
  UpdateWindow(hwnd);

  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(NOTIFYICONDATAA);
  nid.hWnd = hwnd;
  nid.uID = 1; // non-zero id
  nid.guidItem = kTrayGuid;

  // Add with message callback; icon will be set in tray_update below.
  nid.uFlags = NIF_MESSAGE | NIF_GUID;
  nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
  if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
    DestroyWindow(hwnd);
    hwnd = NULL;
    UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandle(NULL));
    return -1;
  }

  // Opt into the modern notify icon behavior for reliable balloon/toast events
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconA(NIM_SETVERSION, &nid);

  tray_update(tray);
  return 0;
}

int tray_loop(int blocking) {
  MSG msg;
  if (blocking) {
    // Get thread-wide messages so we receive WM_QUIT too
    BOOL r = GetMessageA(&msg, NULL, 0, 0);
    if (r <= 0) {
      return -1; // error or WM_QUIT
    }
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
    return 0;
  } else {
    // Drain all pending messages safely
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        return -1;
      }
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    return 0;
  }
}

void tray_update(struct tray *tray) {
  g_tray = tray; // remember the last state for re-adding after Explorer restarts
  UINT id = ID_TRAY_FIRST;
  HMENU prevmenu = hmenu;
  hmenu = _tray_menu(tray->menu, &id);
  SendMessage(hwnd, WM_INITMENUPOPUP, (WPARAM) hmenu, 0);

  HICON icon = _fetch_icon(tray->icon, REGULAR);
  
  // Rebuild flags each update to avoid stale bits carrying over
  DWORD flags = NIF_MESSAGE | NIF_GUID;

  if (icon != NULL) {
    nid.hIcon = icon;
    flags |= NIF_ICON;
  }

  if (tray->tooltip != 0 && strlen(tray->tooltip) > 0) {
    strncpy(nid.szTip, tray->tooltip, sizeof(nid.szTip));
    nid.szTip[sizeof(nid.szTip) - 1] = '\0';
    flags |= NIF_TIP;
#ifdef NIF_SHOWTIP
    // With NOTIFYICON_VERSION_4, standard tooltip can be suppressed unless NIF_SHOWTIP is set.
    flags |= NIF_SHOWTIP;
#endif
  } else {
    nid.szTip[0] = '\0';
  }

  
  // Balloon/toast (legacy surface mapped to Win10+ toasts)
  const BOOL has_title = (tray->notification_title && tray->notification_title[0]);
  const BOOL has_text  = (tray->notification_text  && tray->notification_text[0]);
  if (has_title || has_text) {
    safe_copy_sz(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle),
                 has_title ? tray->notification_title : "");
    safe_copy_sz(nid.szInfo, ARRAYSIZE(nid.szInfo),
                 has_text ? tray->notification_text : "");
    nid.dwInfoFlags = NIIF_NONE;

    // Prefer a user-provided notification icon; else fall back to the tray icon.
    HICON hLarge = NULL;
    if (tray->notification_icon && tray->notification_icon[0]) {
      hLarge = (HICON)LoadImageA(NULL, tray->notification_icon, IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON) * 2,
                                 GetSystemMetrics(SM_CYICON) * 2,
                                 LR_LOADFROMFILE);
    }
    if (!hLarge && nid.hIcon) {
      hLarge = nid.hIcon;
    }
#if defined(NIIF_LARGE_ICON)
    if (hLarge) {
      nid.hBalloonIcon = hLarge;
      nid.dwInfoFlags  = NIIF_USER | NIIF_LARGE_ICON;
    }
#endif
    flags |= NIF_INFO;
  } else {
    // Clear any previous info text to avoid the shell re-showing old balloons
    nid.szInfoTitle[0] = '\0';
    nid.szInfo[0]      = '\0';
    nid.dwInfoFlags    = NIIF_NONE;
  }

  // Keep the callback up-to-date regardless of Focus Assist state
  notification_cb = tray->notification_cb;

  // Apply the freshly computed flags for this modification (prevents stale NIF_* carry-over)
  nid.uFlags = flags;
  Shell_NotifyIconA(NIM_MODIFY, &nid);

  if (prevmenu != NULL) {
    DestroyMenu(prevmenu);
  }
}

void tray_exit(void) {
  Shell_NotifyIconA(NIM_DELETE, &nid);
  _destroy_icon_cache();
  if (hmenu != 0) {
    DestroyMenu(hmenu);
  }
  PostQuitMessage(0);
  UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandle(NULL));
}
