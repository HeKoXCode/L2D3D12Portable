#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using D3DPerfGetStatusFn = DWORD(WINAPI*)();

namespace {

LRESULT CALLBACK TestWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

int Fail(const wchar_t* message, HRESULT result = E_FAIL) {
    fwprintf(stderr, L"%ls (0x%08lX)\n", message, result);
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        fwprintf(
            stderr,
            L"Usage: proxy_smoke_test.exe <proxy-d3d9.dll> [frame-count] "
            L"[expected-backend]\n");
        return 2;
    }
    const int frame_count = argc >= 3 ? _wtoi(argv[2]) : 3;
    const wchar_t* expected_backend = argc == 4 ? argv[3] : nullptr;
    if (frame_count < 1 || frame_count > 10000) {
        fwprintf(stderr, L"frame-count must be between 1 and 10000.\n");
        return 2;
    }

    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) {
        return Fail(L"LoadLibraryW failed", HRESULT_FROM_WIN32(GetLastError()));
    }

    const auto create_d3d9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(proxy, "Direct3DCreate9"));
    if (!create_d3d9) {
        return Fail(
            L"Direct3DCreate9 export was not found",
            HRESULT_FROM_WIN32(GetLastError()));
    }

    const auto create_d3d9_ex = reinterpret_cast<Direct3DCreate9ExFn>(
        GetProcAddress(proxy, "Direct3DCreate9Ex"));
    if (!create_d3d9_ex) {
        return Fail(
            L"Direct3DCreate9Ex export was not found",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    IDirect3D9Ex* d3d9_ex = nullptr;
    HRESULT ex_result = create_d3d9_ex(D3D_SDK_VERSION, &d3d9_ex);
    if (FAILED(ex_result) || !d3d9_ex) {
        return Fail(L"Direct3DCreate9Ex failed", ex_result);
    }
    wprintf(L"Direct3DCreate9Ex adapter_count=%u\n", d3d9_ex->GetAdapterCount());
    d3d9_ex->Release();

    const auto d3dperf_get_status = reinterpret_cast<D3DPerfGetStatusFn>(
        GetProcAddress(proxy, "D3DPERF_GetStatus"));
    if (!d3dperf_get_status) {
        return Fail(
            L"D3DPERF_GetStatus export was not found",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    wprintf(L"D3DPERF_GetStatus=%lu\n", d3dperf_get_status());

    IDirect3D9* d3d9 = create_d3d9(D3D_SDK_VERSION);
    if (!d3d9) {
        return Fail(L"Direct3DCreate9 returned null");
    }

    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = TestWindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"L2D3D12PortableSmokeTest";
    if (!RegisterClassW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        d3d9->Release();
        return Fail(
            L"RegisterClassW failed",
            HRESULT_FROM_WIN32(GetLastError()));
    }

    HWND window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"L2D3D12Portable smoke test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        240,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    if (!window) {
        d3d9->Release();
        return Fail(
            L"CreateWindowExW failed",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    wchar_t hidden_value[8] = {};
    const bool hide_window =
        GetEnvironmentVariableW(
            L"L2D3D12_SMOKE_HIDDEN",
            hidden_value,
            static_cast<DWORD>(std::size(hidden_value))) > 0;
    ShowWindow(window, hide_window ? SW_HIDE : SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    SetFocus(window);
    MSG message = {};
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    D3DDISPLAYMODE display_mode = {};
    HRESULT result =
        d3d9->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display_mode);
    if (FAILED(result)) {
        DestroyWindow(window);
        d3d9->Release();
        return Fail(L"GetAdapterDisplayMode failed", result);
    }

    D3DCAPS9 capabilities = {};
    result = d3d9->GetDeviceCaps(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        &capabilities);
    if (FAILED(result)) {
        DestroyWindow(window);
        d3d9->Release();
        return Fail(L"GetDeviceCaps failed", result);
    }

    const HRESULT device_type_result = d3d9->CheckDeviceType(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        display_mode.Format,
        display_mode.Format,
        TRUE);
    wprintf(
        L"adapter_count=%u display=%ux%u@%u format=%u check_device=0x%08lX "
        L"devcaps=0x%08lX\n",
        d3d9->GetAdapterCount(),
        display_mode.Width,
        display_mode.Height,
        display_mode.RefreshRate,
        static_cast<unsigned int>(display_mode.Format),
        device_type_result,
        capabilities.DevCaps);

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.BackBufferWidth = 320;
    parameters.BackBufferHeight = 240;
    parameters.BackBufferFormat = display_mode.Format;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.MultiSampleQuality = 0;
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.EnableAutoDepthStencil = FALSE;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    IDirect3DDevice9* device = nullptr;
    const DWORD hardware_behavior =
        (capabilities.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)
            ? D3DCREATE_HARDWARE_VERTEXPROCESSING
            : D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    struct DeviceAttempt {
        D3DDEVTYPE type;
        DWORD behavior;
        const wchar_t* label;
    };
    const DeviceAttempt attempts[] = {
        {D3DDEVTYPE_HAL, hardware_behavior, L"HAL"},
        {D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, L"HAL-software-VP"},
        {D3DDEVTYPE_REF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, L"REF"},
        {D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, L"NULLREF"},
    };

    result = D3DERR_NOTAVAILABLE;
    for (const DeviceAttempt& attempt : attempts) {
        device = nullptr;
        result = d3d9->CreateDevice(
            D3DADAPTER_DEFAULT,
            attempt.type,
            window,
            attempt.behavior,
            &parameters,
            &device);
        wprintf(
            L"CreateDevice attempt=%ls result=0x%08lX device=%p\n",
            attempt.label,
            result,
            device);
        if (SUCCEEDED(result)) {
            break;
        }
    }
    if (FAILED(result)) {
        DestroyWindow(window);
        d3d9->Release();
        return Fail(L"CreateDevice failed", result);
    }

    IDirect3DDevice9On12* device_on_12 = nullptr;
    const HRESULT backend_query = device->QueryInterface(
        __uuidof(IDirect3DDevice9On12),
        reinterpret_cast<void**>(&device_on_12));
    const bool is_nine_on_twelve =
        SUCCEEDED(backend_query) && device_on_12 != nullptr;
    if (device_on_12) {
        device_on_12->Release();
    }
    wprintf(
        L"effective_backend=%ls query_result=0x%08lX\n",
        is_nine_on_twelve ? L"9on12" : L"native9",
        backend_query);
    if (expected_backend) {
        const bool expect_nine_on_twelve =
            _wcsicmp(expected_backend, L"9on12") == 0;
        const bool expect_native = _wcsicmp(expected_backend, L"native9") == 0;
        if (!expect_nine_on_twelve && !expect_native) {
            device->Release();
            DestroyWindow(window);
            d3d9->Release();
            return Fail(L"Unknown expected backend");
        }
        if (expect_nine_on_twelve != is_nine_on_twelve) {
            device->Release();
            DestroyWindow(window);
            d3d9->Release();
            return Fail(L"Effective backend did not match expectation");
        }
    }

    wprintf(L"Calling Clear...\n");
    fflush(stdout);
    for (int frame = 0; frame < frame_count; ++frame) {
        result = device->Clear(
            0,
            nullptr,
            D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(12, 24, 48),
            1.0f,
            0);
        if (FAILED(result)) {
            device->Release();
            DestroyWindow(window);
            d3d9->Release();
            return Fail(L"Clear failed", result);
        }
        if (frame < 3 || frame + 1 == frame_count) {
            wprintf(L"Calling Present frame=%d...\n", frame);
            fflush(stdout);
        }
        result = device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result)) {
            device->Release();
            DestroyWindow(window);
            d3d9->Release();
            return Fail(L"Present failed", result);
        }
    }

    device->Release();
    DestroyWindow(window);
    d3d9->Release();
    FreeLibrary(proxy);
    wprintf(L"L2D3D12Portable D3D9 smoke test passed.\n");
    return 0;
}
