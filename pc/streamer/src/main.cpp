// RinCast Streamer entry point: single-instance Win32 shell with a tray icon
// and a WebView2-hosted UI (pc/streamer/ui).
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <memory>
#include <string>

#include "WebView2.h"
#include "app.h"
#include "rc/platform.h"
#include "resource.h"
#include "settings.h"
#include "util.h"

#pragma comment(lib, "dwmapi.lib")

using namespace rcs;
using Microsoft::WRL::Callback;

namespace {

constexpr UINT WM_APP_UI = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_SHOW = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT kCmdOpen = 100, kCmdDisconnect = 101, kCmdQuit = 102;
const wchar_t* kWindowClass = L"RinCastStreamerWindow";

HWND g_hwnd = nullptr;
HINSTANCE g_instance = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
bool g_quitting = false;
bool g_trayHintShown = false;
UINT g_taskbarCreated = 0;

void ApplyWindowStyle(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    // The title bar belongs to the page: black, without a line between them (Windows 11).
    COLORREF black = RGB(0, 0, 0);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &black, sizeof(black));
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(nid.szTip, L"RinCast Streamer");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowMainWindow() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (g_controller) g_controller->put_IsVisible(TRUE);
}

void ResizeWebView() {
    if (!g_controller) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

void InitWebView(HWND hwnd) {
    const auto userData = DataDir() / L"WebView2";
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    MessageBoxW(hwnd,
                                L"RinCast needs the Microsoft Edge WebView2 Runtime.\n"
                                L"Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                                L"RinCast Streamer", MB_ICONERROR);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [hwnd](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                                  if (FAILED(hr) || !controller) return S_OK;
                                  g_controller = controller;
                                  g_controller->get_CoreWebView2(&g_webview);

                                  ComPtr<ICoreWebView2Controller2> c2;
                                  if (SUCCEEDED(g_controller.As(&c2))) {
                                      c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0, 0, 0});
                                  }
                                  ComPtr<ICoreWebView2Settings> settings;
                                  g_webview->get_Settings(&settings);
                                  settings->put_AreDevToolsEnabled(
#ifdef NDEBUG
                                      FALSE
#else
                                      TRUE
#endif
                                  );
                                  settings->put_AreDefaultContextMenusEnabled(FALSE);
                                  settings->put_IsStatusBarEnabled(FALSE);
                                  settings->put_IsZoomControlEnabled(FALSE);
                                  ComPtr<ICoreWebView2Settings3> s3;
                                  if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

                                  ComPtr<ICoreWebView2_3> wv3;
                                  if (SUCCEEDED(g_webview.As(&wv3))) {
                                      std::filesystem::path ui = ExeDir() / L"ui";
                                      if (!std::filesystem::exists(ui / L"index.html"))
                                          ui = ExeDir() / L".." / L".." / L"ui";  // dev tree
                                      wv3->SetVirtualHostNameToFolderMapping(
                                          L"app.rincast", ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                  }
                                  g_webview->add_WebMessageReceived(
                                      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                          [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                              LPWSTR message = nullptr;
                                              if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
                                                  App::Get().HandleUiMessage(Narrow(message));
                                                  CoTaskMemFree(message);
                                              }
                                              return S_OK;
                                          })
                                          .Get(),
                                      nullptr);
                                  // Keep navigation inside the app; open links externally.
                                  g_webview->add_NewWindowRequested(
                                      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                              LPWSTR uri = nullptr;
                                              args->get_Uri(&uri);
                                              if (uri) {
                                                  ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                                                  CoTaskMemFree(uri);
                                              }
                                              args->put_Handled(TRUE);
                                              return S_OK;
                                          })
                                          .Get(),
                                      nullptr);
                                  ResizeWebView();
                                  g_webview->Navigate(L"https://app.rincast/index.html");
                                  return S_OK;
                              })
                              .Get());
                return S_OK;
            })
            .Get());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon(hwnd);  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            ApplyWindowStyle(hwnd);
            AddTrayIcon(hwnd);
            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = GetDpiForWindow(hwnd);
            mmi->ptMinTrackSize.x = MulDiv(760, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(560, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            App::Get().Tick();
            return 0;
        case WM_APP_UI: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (g_webview) g_webview->PostWebMessageAsJson(text->c_str());
            return 0;
        }
        case WM_APP_SHOW:
            ShowMainWindow();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                    ShowMainWindow();
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP: {
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open RinCast");
                    if (App::Get().Server().ActiveSession()) AppendMenuW(menu, MF_STRING, kCmdDisconnect, L"Disconnect headset");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit");
                    POINT pt;
                    GetCursorPos(&pt);
                    SetForegroundWindow(hwnd);
                    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    break;
                }
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdOpen: ShowMainWindow(); break;
                case kCmdDisconnect: App::Get().HandleUiMessage(R"({"type":"disconnect"})"); break;
                case kCmdQuit:
                    g_quitting = true;
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        case WM_CLOSE:
            if (!g_quitting) {
                // Keep streaming in the background, like a service.
                ShowWindow(hwnd, SW_HIDE);
                if (g_controller) g_controller->put_IsVisible(FALSE);
                if (!g_trayHintShown) {
                    g_trayHintShown = true;
                    TrayBalloon(L"RinCast is still running", L"Your headset can connect at any time. Quit from the tray icon.");
                }
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_ENDSESSION:
            g_quitting = true;
            return 0;
        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            NOTIFYICONDATAW nid{sizeof(nid)};
            nid.hWnd = hwnd;
            nid.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            App::Get().SetUiSink(nullptr);
            g_controller.Reset();
            g_webview.Reset();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    g_instance = instance;
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\RinCast.Streamer.Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, WM_APP_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Streaming is real-time work.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    if (!App::Get().Init()) {
        MessageBoxW(nullptr, L"RinCast Streamer could not start its network service. Is another copy running?",
                    L"RinCast Streamer", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    const UINT dpi = GetDpiForSystem();
    // The size of what's inside the window: Streaming fits without scrolling, status bar included.
    RECT size{0, 0, MulDiv(900, dpi, 96), MulDiv(690, dpi, 96)};
    AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = size.right - size.left, h = size.bottom - size.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_hwnd = CreateWindowExW(0, kWindowClass, L"RinCast Streamer", WS_OVERLAPPEDWINDOW,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                             w, h, nullptr, nullptr, instance, nullptr);

    App::Get().SetUiSink([](const std::string& json) {
        auto* text = new std::wstring(Widen(json));
        if (!PostMessageW(g_hwnd, WM_APP_UI, 0, reinterpret_cast<LPARAM>(text))) delete text;
    });
    App::Get().ShowWindowRequest = [] { PostMessageW(g_hwnd, WM_APP_SHOW, 0, 0); };

    const bool minimized = cmdLine && wcsstr(cmdLine, L"--minimized") != nullptr;
    InitWebView(g_hwnd);
    if (!minimized) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    } else {
        g_trayHintShown = true;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    App::Get().Shutdown();
    CoUninitialize();
    if (singleton) CloseHandle(singleton);
    return 0;
}
