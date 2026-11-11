// who: a small WhatsApp client. The chats come from who-engine (whatsmeow,
// run hidden); the window is Dear ImGui on Direct3D 11, frameless. Closing
// it leaves Who in the tray, as WhatsApp's own app does; Quit is in the
// tray icon's menu.
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "net.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 3;  // a second start
constexpr UINT WM_APP_QUIT = WM_APP + 5;  // from the settings menu
constexpr UINT WM_APP_TRAY = WM_APP + 6;
constexpr UINT kTrayOpen = 1, kTrayQuit = 2;
const wchar_t* kClassName = L"who";

ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapChain = nullptr;
ID3D11RenderTargetView* target = nullptr;
UINT resizeW = 0, resizeH = 0;

HWND hwnd = nullptr;
HINSTANCE instance = nullptr;
std::unique_ptr<who::App> app;
int framesWanted = 2;
bool quitting = false;
NOTIFYICONDATAW tray{};
HICON iconPlain = nullptr, iconUnread = nullptr;
int shownUnread = -1;
ITaskbarList3* taskbar = nullptr;
UINT taskbarCreated = 0;

void createTarget() {
    ID3D11Texture2D* back = nullptr;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back, nullptr, &target);
    back->Release();
}

void releaseTarget() {
    if (target) target->Release();
    target = nullptr;
}

bool createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) return false;
    createTarget();
    return true;
}

void destroyDevice() {
    releaseTarget();
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
}

void renderFrame() {
    static bool rendering = false;
    if (rendering || !app) return;
    rendering = true;
    if (resizeW != 0) {
        releaseTarget();
        swapChain->ResizeBuffers(0, resizeW, resizeH, DXGI_FORMAT_UNKNOWN, 0);
        resizeW = resizeH = 0;
        createTarget();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (app->BeforeFrame()) framesWanted = 3;
    ImGui::NewFrame();
    app->Frame();
    ImGui::Render();
    const ImVec4& bg = who::Colours().bg;
    const float clear[4] = {bg.x, bg.y, bg.z, 1};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain->Present(1, 0);
    rendering = false;
}

// unreadIcon draws the app icon with a green dot in the corner, for the
// tray when there's something unread.
HICON unreadIcon(HICON base, int size) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP colour = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, colour);
    memset(bits, 0, size_t(size) * size * 4);
    DrawIconEx(dc, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);
    // The dot, with an outline so it reads on any taskbar.
    auto* px = static_cast<unsigned*>(bits);
    float r = size * 0.24f, cx = size - r - 0.5f, cy = size - r - 0.5f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d <= r) px[y * size + x] = 0xFF3FB950;
            else if (d <= r + 1.2f) px[y * size + x] = 0xFF0B0C0F;
        }
    SelectObject(dc, old);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, colour};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(colour);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void addTray() {
    tray.cbSize = sizeof(tray);
    tray.hWnd = hwnd;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = WM_APP_TRAY;
    tray.hIcon = iconPlain;
    wcscpy_s(tray.szTip, L"Who");
    Shell_NotifyIconW(NIM_ADD, &tray);
    shownUnread = -1;
}

// updateUnread shows unread messages on the tray icon and the taskbar.
void updateUnread() {
    int n = app ? app->Unread() : 0;
    if (n == shownUnread) return;
    shownUnread = n;
    tray.hIcon = n > 0 ? iconUnread : iconPlain;
    if (n > 0) swprintf_s(tray.szTip, L"Who: %d unread", n);
    else wcscpy_s(tray.szTip, L"Who");
    Shell_NotifyIconW(NIM_MODIFY, &tray);
    if (taskbar) taskbar->SetOverlayIcon(hwnd, n > 0 ? iconUnread : nullptr, n > 0 ? L"Unread messages" : L"");
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kTrayOpen, L"Open Who");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kTrayQuit, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);  // so the menu closes when clicking away
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == kTrayOpen && app) app->ShowWindow();
    if (cmd == kTrayQuit) {
        quitting = true;
        DestroyWindow(hwnd);
    }
}

LRESULT hitTest(HWND h, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(h, &pt);
    RECT rc;
    GetClientRect(h, &rc);
    UINT dpi = GetDpiForWindow(h);
    int band = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    int corner = band * 2;
    bool left = pt.x < band, right = pt.x >= rc.right - band;
    bool top = pt.y < band, bottom = pt.y >= rc.bottom - band;
    bool nearLeft = pt.x < corner, nearRight = pt.x >= rc.right - corner;
    bool nearTop = pt.y < corner, nearBottom = pt.y >= rc.bottom - corner;
    if (!IsZoomed(h)) {
        if ((top && nearLeft) || (left && nearTop)) return HTTOPLEFT;
        if ((top && nearRight) || (right && nearTop)) return HTTOPRIGHT;
        if ((bottom && nearLeft) || (left && nearBottom)) return HTBOTTOMLEFT;
        if ((bottom && nearRight) || (right && nearBottom)) return HTBOTTOMRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }
    if (app && app->InCaption(float(pt.x), float(pt.y))) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wParam, lParam)) return true;
    if (msg == taskbarCreated && taskbarCreated) {
        addTray();  // Explorer restarted
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) {
            if (IsZoomed(h)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                UINT dpi = GetDpiForWindow(h);
                int f = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += f;
                p->rgrc[0].right -= f;
                p->rgrc[0].top += f;
                p->rgrc[0].bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return hitTest(h, lParam);
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resizeW = LOWORD(lParam);
            resizeH = HIWORD(lParam);
            if (IsWindowVisible(h)) renderFrame();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (app && !IsZoomed(h)) {
            RECT r;
            GetWindowRect(h, &r);
            app->SavePlace(r);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(h);
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = {LONG(560 * scale), LONG(360 * scale)};
        return 0;
    }
    case WM_DPICHANGED: {
        if (app) app->SetScale(float(HIWORD(wParam)) / 96.0f);
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = HDROP(wParam);
        std::vector<std::string> paths;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH];
            if (DragQueryFileW(drop, i, p, MAX_PATH)) paths.push_back(who::Narrow(p));
        }
        DragFinish(drop);
        if (app) app->DropFiles(paths);
        return 0;
    }
    case WM_CLOSE:
        // To the tray, like WhatsApp; Quit is in the tray icon's menu.
        if (!quitting) {
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        break;
    case WM_APP_QUIT:
        quitting = true;
        DestroyWindow(h);
        return 0;
    case WM_APP_SHOW:
        if (app) app->ShowWindow();
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (IsWindowVisible(h) && !IsIconic(h) && GetForegroundWindow() == h) ShowWindow(h, SW_HIDE);
            else if (app) app->ShowWindow();
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            trayMenu();
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    instance = inst;
    // -tray starts it hidden in the tray (for starting with Windows).
    bool startHidden = wcsstr(cmdLine, L"-tray") != nullptr;
    std::wstring mutexName = L"Local\\who";
    bool testing = false;
    if (const wchar_t* d = _wgetenv(L"WHO_DATA"); d && *d) {
        testing = true;
        mutexName += L"-test";
    }
    RECT forced{};
    bool force = false;
    if (const wchar_t* p = wcsstr(cmdLine, L"-place ")) {
        int x, y, w, h;
        if (swscanf(p + 7, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
            forced = RECT{x, y, x + w, y + h};
            force = true;
        }
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kClassName, nullptr); other && !startHidden) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = int(1000 * scale), h = int(700 * scale);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    hwnd = CreateWindowExW(0, kClassName, L"Who", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    MARGINS margins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    DragAcceptFiles(hwnd, TRUE);
    if (!createDevice()) {
        MessageBoxW(nullptr, L"Couldn't start Direct3D 11.", L"Who", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    app = std::make_unique<who::App>(hwnd, device, [] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });
    app->SetScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));
    RECT placed;
    if (force) placed = forced;
    if (force || app->Place(placed))
        SetWindowPos(hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    int traySize = GetSystemMetrics(SM_CXSMICON);
    iconPlain = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, traySize, traySize, 0);
    iconUnread = unreadIcon(iconPlain, traySize);
    addTray();
    CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
    if (taskbar) taskbar->HrInit();

    if (testing) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    } else if (!startHidden) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    bool running = true;
    while (running) {
        bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        DWORD timeout = framesWanted > 0 ? 0 : visible && app->Animating() ? 33 : visible ? 1000 : 5000;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            framesWanted = 2;
        }
        if (!running) break;
        app->Pump();
        updateUnread();
        if (!visible) continue;
        renderFrame();
        if (framesWanted > 0) framesWanted--;
    }

    app.reset();
    if (taskbar) taskbar->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    CoUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
