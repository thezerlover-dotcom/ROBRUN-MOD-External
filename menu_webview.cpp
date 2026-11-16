/* menu_webview.cpp - WebView2 host for the embedded settings page.
 *
 * A dedicated window thread hosts the page compiled into app.rc. BR_Config
 * holds the settings shared with the overlay. Messages are strings:
 *   HTML -> host: set:<key>:<int>, col:<index>:r,g,b, act:save|load,
 *                 drag:<x>,<y>,<dx>,<dy>, dragend:, close:, hello:
 *   host -> HTML: state:<json>, toast:<text>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

#include "menu_bridge.h"
#include "app_resources.h"
#include "wv2_cb.h"

#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <combaseapi.h>
#include "WebView2.h"
#include "wv2_cb_impl.h"
#pragma comment(lib, "WebView2Loader.dll.lib")

static HWND g_hwnd = NULL;
static ICoreWebView2Environment* g_env = nullptr;
static ICoreWebView2* g_webview = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;

static HANDLE g_thread = NULL;
static DWORD g_thread_id = 0;
static volatile LONG g_quit = 0;
static volatile LONG g_visible = 1;
static bool g_dragging = false;
static bool g_page_ready = false;
static HRESULT g_start_error = S_OK;
static HWND g_previous_foreground = NULL;
static POINT g_drag_origin = { 0, 0 };

static const int MENU_WIDTH = 976;
static const int MENU_HEIGHT = 640;

static void menu_failed(const char* stage, HRESULT hr) {
    (void)stage;
    g_start_error = FAILED(hr) ? hr : E_FAIL;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}

static HRESULT navigate_menu(ICoreWebView2* webview) {
    HMODULE module = GetModuleHandleW(NULL);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_MENU_HTML),
                                   MAKEINTRESOURCEW(10));
    if (!resource) return E_FAIL;

    HGLOBAL data = LoadResource(module, resource);
    const char* bytes = static_cast<const char*>(LockResource(data));
    int size = (int)SizeofResource(module, resource);
    if (!bytes || size <= 0) return E_FAIL;

    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, NULL, 0);
    if (!count) return E_FAIL;

    std::wstring html((size_t)count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size,
                             &html[0], count))
        return E_FAIL;
    return webview->NavigateToString(html.c_str());
}

static void json_int(std::string& json, const char* key, int value, bool first = false) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s\"%s\":%d", first ? "" : ",", key, value);
    json += buffer;
}

static std::string build_state_json(BR_Config* config) {
    std::string json("{");
#define BR_JSON(field, initial, minimum, maximum) \
    json_int(json, #field, config->field, json.size() == 1);
    BR_SETTINGS(BR_JSON)
#undef BR_JSON

    json += ",\"colors\":{";
    for (int i = 0; i < BR_COLOR_COUNT; ++i) {
        char rgb[48];
        snprintf(rgb, sizeof(rgb), "%s\"%d\":[%u,%u,%u]", i ? "," : "", i,
                 (unsigned)config->color[i].rgb[0],
                 (unsigned)config->color[i].rgb[1],
                 (unsigned)config->color[i].rgb[2]);
        json += rgb;
    }
    json += "}}";
    return json;
}

static void wv_send(const std::string& message) {
    if (!g_webview) return;
    std::wstring wide(message.begin(), message.end());
    g_webview->PostWebMessageAsString(wide.c_str());
}

static bool parse_int(const std::string& text, int* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    long parsed = strtol(text.c_str(), &end, 0);
    if (end == text.c_str() || *end) return false;
    *value = (int)parsed;
    return true;
}

static HRESULT STDMETHODCALLTYPE message_received(
    ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args, void* context) {
    (void)sender;
    (void)context;
    if (!args) return S_OK;

    LPWSTR wide = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&wide)) || !wide) return S_OK;

    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    std::string message;
    if (length > 1) {
        message.resize((size_t)length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, &message[0], length, NULL, NULL);
        message.resize((size_t)length - 1);
    }
    CoTaskMemFree(wide);

    size_t separator = message.find(':');
    if (separator == std::string::npos) return S_OK;
    std::string command = message.substr(0, separator);
    std::string body = message.substr(separator + 1);
    BR_Config* config = br_get();

    if (command == "set") {
        size_t key_end = body.find(':');
        int value = 0;
        if (key_end != std::string::npos && parse_int(body.substr(key_end + 1), &value))
            br_config_set(config, body.substr(0, key_end).c_str(), value);
    } else if (command == "col") {
        int index, red, green, blue;
        if (sscanf(body.c_str(), "%d:%d,%d,%d", &index, &red, &green, &blue) == 4 &&
            index >= 0 && index < BR_COLOR_COUNT &&
            (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
            config->color[index].rgb[0] = (unsigned char)red;
            config->color[index].rgb[1] = (unsigned char)green;
            config->color[index].rgb[2] = (unsigned char)blue;
        }
    } else if (command == "act") {
        if (body == "save") {
            wv_send(br_config_save() ? "toast:Settings saved"
                                     : "toast:Could not save RobRun.ini");
        } else if (body == "load") {
            if (br_config_load()) {
                wv_send("state:" + build_state_json(config));
                wv_send("toast:Settings loaded");
            } else {
                wv_send("toast:Could not load RobRun.ini");
            }
        }
    } else if (command == "drag") {
        int sx = 0, sy = 0, dx = 0, dy = 0;
        if (sscanf(body.c_str(), "%d,%d,%d,%d", &sx, &sy, &dx, &dy) == 4) {
            if (!g_dragging) {
                g_dragging = true;
                RECT window;
                GetWindowRect(g_hwnd, &window);
                g_drag_origin.x = window.left;
                g_drag_origin.y = window.top;
            }
            SetWindowPos(g_hwnd, NULL, g_drag_origin.x + dx, g_drag_origin.y + dy,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            (void)sx;
            (void)sy;
        }
    } else if (command == "dragend") {
        g_dragging = false;
        g_drag_origin.x = g_drag_origin.y = 0;
    } else if (command == "close") {
        br_set_menu_visible(0);
    } else if (command == "hello") {
        g_page_ready = true;
        g_start_error = S_OK;
        wv_send("state:" + build_state_json(config));
    }
    return S_OK;
}

static LRESULT WINAPI menu_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        if (!g_page_ready) {
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(231, 234, 241));
            SetBkMode(dc, TRANSPARENT);
            const wchar_t* text = FAILED(g_start_error)
                ? L"RobRun could not start the settings page.\n"
                  L"Install the Microsoft Edge WebView2 Runtime and press F1 to close."
                : L"Loading RobRun settings...";
            DrawTextW(dc, text, -1, &client,
                      DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void configure_webview(ICoreWebView2* webview) {
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(webview->get_Settings(&settings)) || !settings) return;
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    ICoreWebView2Settings3* settings3 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3),
                                           reinterpret_cast<void**>(&settings3))) &&
        settings3) {
        settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        settings3->Release();
    }
    settings->Release();
}

static HRESULT controller_ready(HRESULT hr, ICoreWebView2Controller* controller) {
    if (FAILED(hr) || !controller) {
        menu_failed("create browser controller", hr);
        return S_OK;
    }

    g_controller = controller;
    g_controller->AddRef();

    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    g_controller->put_Bounds(bounds);
    g_controller->put_IsVisible(IsWindowVisible(g_hwnd));

    hr = g_controller->get_CoreWebView2(&g_webview);
    if (SUCCEEDED(hr) && g_webview) {
        configure_webview(g_webview);
        auto handler = MakeMsgCb([](ICoreWebView2* sender,
                                    ICoreWebView2WebMessageReceivedEventArgs* args) {
            return message_received(sender, args, nullptr);
        });
        EventRegistrationToken token{};
        hr = g_webview->add_WebMessageReceived(handler, &token);
        handler->Release();
        if (SUCCEEDED(hr)) hr = navigate_menu(g_webview);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }

    if (FAILED(hr)) {
        g_controller->put_IsVisible(FALSE);
        menu_failed("open embedded menu", hr);
    }
    return S_OK;
}

static void start_webview(const wchar_t* data_directory, bool allow_retry);

static void browser_start_failed(HRESULT hr, bool allow_retry) {
    wchar_t fallback[MAX_PATH];
    DWORD length = GetTempPathW(_countof(fallback), fallback);
    if (allow_retry && length && length < _countof(fallback) &&
        swprintf_s(fallback + length, _countof(fallback) - length,
                   L"RobRunWebView2_%lu", GetCurrentProcessId()) >= 0) {
        start_webview(fallback, false);
        return;
    }
    menu_failed("create browser environment", hr);
}

static void start_webview(const wchar_t* data_directory, bool allow_retry) {
    auto handler = MakeEnvCb([allow_retry](HRESULT hr,
                                          ICoreWebView2Environment* environment) -> HRESULT {
        if (FAILED(hr) || !environment) {
            browser_start_failed(FAILED(hr) ? hr : E_FAIL, allow_retry);
            return S_OK;
        }
        g_env = environment;
        g_env->AddRef();
        auto controller = MakeCtlCb(controller_ready);
        hr = g_env->CreateCoreWebView2Controller(g_hwnd, controller);
        controller->Release();
        if (FAILED(hr)) menu_failed("request browser controller", hr);
        return S_OK;
    });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data_directory,
                                                          nullptr, handler);
    handler->Release();
    if (FAILED(hr)) browser_start_failed(hr, allow_retry);
}

static int ensure_directory(const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return CreateDirectoryW(path, NULL) != 0;
}

static DWORD WINAPI menu_thread(LPVOID unused) {
    (void)unused;

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;

    WNDCLASSEXW window_class = { 0 };
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = menu_window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"RobRunMenuWindow";
    window_class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&window_class);

    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        window_class.lpszClassName, L"RobRun",
        WS_POPUP,
        screen_width / 2 - MENU_WIDTH / 2,
        (screen_height - MENU_HEIGHT) / 2,
        MENU_WIDTH, MENU_HEIGHT,
        NULL, NULL, window_class.hInstance, NULL);
    if (!g_hwnd) {
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        CoUninitialize();
        return 1;
    }

    wchar_t data_directory[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", data_directory,
                                           _countof(data_directory));
    bool directory_ok = false;
    if (length && length < _countof(data_directory) &&
        swprintf_s(data_directory + length, _countof(data_directory) - length,
                   L"\\RobRun") >= 0) {
        directory_ok = ensure_directory(data_directory);
        if (directory_ok &&
            swprintf_s(data_directory + length, _countof(data_directory) - length,
                       L"\\RobRun\\WebView2") >= 0)
            directory_ok = ensure_directory(data_directory);
    }

    if (!directory_ok) {
        length = GetTempPathW(_countof(data_directory), data_directory);
        directory_ok = length && length < _countof(data_directory) &&
                       swprintf_s(data_directory + length,
                                  _countof(data_directory) - length,
                                  L"RobRunWebView2_%lu",
                                  GetCurrentProcessId()) >= 0;
    }

    if (directory_ok) start_webview(data_directory, true);
    else menu_failed("resolve browser profile directory", E_FAIL);

    DWORD shown_at = 0;
    MSG message;
    ZeroMemory(&message, sizeof(message));
    while (message.message != WM_QUIT && !g_quit) {
        if (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        if (!InterlockedCompareExchange(&g_visible, 0, 0)) {
            if (IsWindowVisible(g_hwnd)) {
                if (g_controller) g_controller->put_IsVisible(FALSE);
                ShowWindow(g_hwnd, SW_HIDE);
                if (g_previous_foreground && IsWindow(g_previous_foreground))
                    SetForegroundWindow(g_previous_foreground);
                g_previous_foreground = NULL;
            }
            MsgWaitForMultipleObjectsEx(0, NULL, 80, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (!IsWindowVisible(g_hwnd)) {
            shown_at = GetTickCount();
            g_previous_foreground = GetForegroundWindow();
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
            if (g_controller && SUCCEEDED(g_start_error))
                g_controller->put_IsVisible(TRUE);
        }

        DWORD now = GetTickCount();
        if (!g_page_ready && SUCCEEDED(g_start_error) && now - shown_at > 15000) {
            if (g_controller) g_controller->put_IsVisible(FALSE);
            menu_failed("wait for embedded menu", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        MsgWaitForMultipleObjectsEx(0, NULL, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (g_controller) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
    }
    if (g_webview) {
        g_webview->Release();
        g_webview = nullptr;
    }
    if (g_env) {
        g_env->Release();
        g_env = nullptr;
    }
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    UnregisterClassW(L"RobRunMenuWindow", GetModuleHandleW(NULL));
    CoUninitialize();
    return 0;
}

int br_start_menu(void) {
    if (g_thread) return 0;
    g_quit = 0;
    g_thread = CreateThread(NULL, 0, menu_thread, NULL, 0, &g_thread_id);
    return g_thread ? 0 : 1;
}

void br_stop_menu(void) {
    if (!g_thread) return;
    g_quit = 1;
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

void br_request_toggle(void) {
    br_set_menu_visible(!InterlockedCompareExchange(&g_visible, 0, 0));
}

void br_set_menu_visible(int visible) {
    br_get()->menu_open = visible != 0;
    InterlockedExchange(&g_visible, visible != 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
