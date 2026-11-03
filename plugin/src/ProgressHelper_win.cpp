// TrackProgress.exe: a small window that stays on top and shows how far subject tracking (or
// the import) has got. Windows counterpart of ProgressHelper.mm.
//
// Resolve's own interface is busy while the plugin tracks, so the plugin starts this as a
// separate program inside its bundle and feeds it lines on stdin:
//   p <fraction 0..1> <text>    update the bar and the text (UTF-8)
//   done                        close
// Pressing Cancel prints "cancel" on stdout. The window also closes if stdin closes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
const UINT WM_PROGRESS = WM_APP + 1; // wParam: fraction * 10000, lParam: new std::wstring*
const UINT WM_FINISHED = WM_APP + 2;
const int kIdCancel = 100;

HWND s_Window = nullptr, s_Label = nullptr, s_Bar = nullptr, s_Button = nullptr;
HFONT s_Font = nullptr;
bool s_Stopping = false;

std::wstring Widen(const std::string& p_Utf8)
{
    if (p_Utf8.empty())
    {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, p_Utf8.data(), int(p_Utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_Utf8.data(), int(p_Utf8.size()), &out[0], n);
    return out;
}

// Reads the commands on stdin and hands them to the window.
DWORD WINAPI ReadCommands(void*)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string pending;
    char buffer[1024];
    DWORD got = 0;
    bool done = false;
    while (!done && ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got > 0)
    {
        pending.append(buffer, got);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.compare(0, 4, "done") == 0)
            {
                done = true;
                break;
            }
            double fraction = 0.0;
            int used = 0;
            if (sscanf(line.c_str(), "p %lf %n", &fraction, &used) >= 1)
            {
                auto* text = new std::wstring(Widen(line.substr(size_t(used))));
                const WPARAM scaled = WPARAM(fraction < 0.0 ? 0 : fraction > 1.0 ? 10000 : int(fraction * 10000.0 + 0.5));
                if (!PostMessageW(s_Window, WM_PROGRESS, scaled, reinterpret_cast<LPARAM>(text)))
                {
                    delete text;
                }
            }
        }
    }
    PostMessageW(s_Window, WM_FINISHED, 0, 0);
    return 0;
}

LRESULT CALLBACK WindowProc(HWND p_Window, UINT p_Message, WPARAM p_W, LPARAM p_L)
{
    switch (p_Message)
    {
    case WM_COMMAND:
        if (LOWORD(p_W) == kIdCancel && !s_Stopping)
        {
            s_Stopping = true;
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "cancel\n", 7, &written, nullptr);
            SetWindowTextW(s_Label, L"Stopping…");
            EnableWindow(s_Button, FALSE);
        }
        return 0;
    case WM_PROGRESS:
    {
        std::wstring* text = reinterpret_cast<std::wstring*>(p_L);
        SendMessageW(s_Bar, PBM_SETPOS, p_W, 0);
        if (!s_Stopping)
        {
            SetWindowTextW(s_Label, text->c_str());
        }
        delete text;
        return 0;
    }
    case WM_FINISHED:
        DestroyWindow(p_Window);
        return 0;
    case WM_CLOSE:
        // Closing the window counts as Cancel; the plugin then ends it.
        SendMessageW(p_Window, WM_COMMAND, kIdCancel, 0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(p_Window, p_Message, p_W, p_L);
    }
}
}

int WINAPI wWinMain(HINSTANCE p_Instance, HINSTANCE, PWSTR, int)
{
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring title = argv && argc > 1 ? argv[1] : L"Insta360 Stitch";

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = p_Instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"Insta360StitchProgress";
    RegisterClassW(&windowClass);

    // Sizes in pixels at 96 dpi, scaled to the screen.
    HDC screen = GetDC(nullptr);
    const double scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);
    auto px = [scale](int p_Value) { return int(p_Value * scale + 0.5); };

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    s_Font = CreateFontIndirectW(&metrics.lfMessageFont);

    RECT frame = {0, 0, px(400), px(112)};
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    s_Window = CreateWindowExW(exStyle, windowClass.lpszClassName, title.c_str(), style, x, y, width, height, nullptr, nullptr, p_Instance,
                               nullptr);
    s_Label = CreateWindowExW(0, L"STATIC", L"Starting…", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, px(20), px(16),
                              px(360), px(20), s_Window, nullptr, p_Instance, nullptr);
    s_Bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, px(20), px(42), px(360), px(16), s_Window, nullptr,
                            p_Instance, nullptr);
    SendMessageW(s_Bar, PBM_SETRANGE32, 0, 10000);
    s_Button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, px(290), px(72), px(90), px(28),
                               s_Window, reinterpret_cast<HMENU>(INT_PTR(kIdCancel)), p_Instance, nullptr);
    for (HWND control : {s_Label, s_Button})
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(s_Font), TRUE);
    }
    ShowWindow(s_Window, SW_SHOWNOACTIVATE);
    UpdateWindow(s_Window);

    HANDLE reader = CreateThread(nullptr, 0, ReadCommands, nullptr, 0, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(s_Window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    // The reader may still wait on stdin; the process ends anyway.
    if (reader)
    {
        CloseHandle(reader);
    }
    DeleteObject(s_Font);
    LocalFree(argv);
    return 0;
}
