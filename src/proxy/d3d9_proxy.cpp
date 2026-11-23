#include <windows.h>
#include <d3d9on12.h>

#include <cstdint>
#include <cwchar>
#include <string>

#include "common/log.h"
#include "proxy/d3d9_hooks.h"

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(unsigned int);
using Direct3DCreate9ExFn =
    HRESULT(WINAPI*)(unsigned int, IDirect3D9Ex**);
using Direct3DCreate9On12Fn =
    IDirect3D9*(WINAPI*)(unsigned int, D3D9ON12_ARGS*, unsigned int);
using Direct3DCreate9On12ExFn = HRESULT(WINAPI*)(
    unsigned int,
    D3D9ON12_ARGS*,
    unsigned int,
    IDirect3D9Ex**);
using D3DPerfBeginEventFn = int(WINAPI*)(D3DCOLOR, LPCWSTR);
using D3DPerfEndEventFn = int(WINAPI*)();
using D3DPerfGetStatusFn = DWORD(WINAPI*)();
using D3DPerfQueryRepeatFrameFn = BOOL(WINAPI*)();
using D3DPerfSetMarkerFn = void(WINAPI*)(D3DCOLOR, LPCWSTR);
using D3DPerfSetOptionsFn = void(WINAPI*)(DWORD);
using D3DPerfSetRegionFn = void(WINAPI*)(D3DCOLOR, LPCWSTR);

namespace {

enum class D3DBackend {
    kNative9,
    kNineOnTwelve,
};

INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real_d3d9 = nullptr;
Direct3DCreate9Fn g_real_direct3d_create9 = nullptr;
Direct3DCreate9ExFn g_real_direct3d_create9_ex = nullptr;
Direct3DCreate9On12Fn g_real_direct3d_create9_on12 = nullptr;
Direct3DCreate9On12ExFn g_real_direct3d_create9_on12_ex = nullptr;
D3DPerfBeginEventFn g_real_d3dperf_begin_event = nullptr;
D3DPerfEndEventFn g_real_d3dperf_end_event = nullptr;
D3DPerfGetStatusFn g_real_d3dperf_get_status = nullptr;
D3DPerfQueryRepeatFrameFn g_real_d3dperf_query_repeat_frame = nullptr;
D3DPerfSetMarkerFn g_real_d3dperf_set_marker = nullptr;
D3DPerfSetOptionsFn g_real_d3dperf_set_options = nullptr;
D3DPerfSetRegionFn g_real_d3dperf_set_region = nullptr;

template <typename Function>
Function ResolveExport(const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(g_real_d3d9, name));
}

const wchar_t* BackendName(D3DBackend backend) {
    return backend == D3DBackend::kNineOnTwelve ? L"9on12" : L"native9";
}

D3DBackend RequestedBackend() {
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"INTERLUDE_D3D_BACKEND",
        value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0) {
        return D3DBackend::kNative9;
    }
    if (length >= std::size(value)) {
        interlude_modern::AppendLog(
            L"INTERLUDE_D3D_BACKEND is too long; using native9");
        return D3DBackend::kNative9;
    }
    if (_wcsicmp(value, L"9on12") == 0) {
        return D3DBackend::kNineOnTwelve;
    }
    if (_wcsicmp(value, L"native9") != 0) {
        wchar_t message[160] = {};
        _snwprintf_s(
            message,
            _TRUNCATE,
            L"Unknown INTERLUDE_D3D_BACKEND=%ls; using native9",
            value);
        interlude_modern::AppendLog(message);
    }
    return D3DBackend::kNative9;
}

BOOL CALLBACK LoadRealD3D9(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t system_directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        interlude_modern::AppendLog(L"GetSystemDirectoryW failed");
        return TRUE;
    }

    std::wstring real_path(system_directory);
    real_path += L"\\d3d9.dll";
    g_real_d3d9 = LoadLibraryW(real_path.c_str());
    if (!g_real_d3d9) {
        interlude_modern::AppendLog(
            L"LoadLibraryW for the system d3d9.dll failed");
        return TRUE;
    }

    g_real_direct3d_create9 =
        ResolveExport<Direct3DCreate9Fn>("Direct3DCreate9");
    if (!g_real_direct3d_create9) {
        interlude_modern::AppendLog(
            L"GetProcAddress(Direct3DCreate9) failed");
        return TRUE;
    }
    g_real_direct3d_create9_ex =
        ResolveExport<Direct3DCreate9ExFn>("Direct3DCreate9Ex");
    g_real_direct3d_create9_on12 =
        ResolveExport<Direct3DCreate9On12Fn>("Direct3DCreate9On12");
    g_real_direct3d_create9_on12_ex =
        ResolveExport<Direct3DCreate9On12ExFn>("Direct3DCreate9On12Ex");
    g_real_d3dperf_begin_event =
        ResolveExport<D3DPerfBeginEventFn>("D3DPERF_BeginEvent");
    g_real_d3dperf_end_event =
        ResolveExport<D3DPerfEndEventFn>("D3DPERF_EndEvent");
    g_real_d3dperf_get_status =
        ResolveExport<D3DPerfGetStatusFn>("D3DPERF_GetStatus");
    g_real_d3dperf_query_repeat_frame =
        ResolveExport<D3DPerfQueryRepeatFrameFn>("D3DPERF_QueryRepeatFrame");
    g_real_d3dperf_set_marker =
        ResolveExport<D3DPerfSetMarkerFn>("D3DPERF_SetMarker");
    g_real_d3dperf_set_options =
        ResolveExport<D3DPerfSetOptionsFn>("D3DPERF_SetOptions");
    g_real_d3dperf_set_region =
        ResolveExport<D3DPerfSetRegionFn>("D3DPERF_SetRegion");

    wchar_t message[160] = {};
    _snwprintf_s(
        message,
        _TRUNCATE,
        L"System d3d9.dll loaded Direct3DCreate9On12=%ls "
        L"Direct3DCreate9On12Ex=%ls",
        g_real_direct3d_create9_on12 ? L"available" : L"unavailable",
        g_real_direct3d_create9_on12_ex ? L"available" : L"unavailable");
    interlude_modern::AppendLog(message);
    return TRUE;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(
    unsigned int sdk_version,
    IDirect3D9Ex** output) {
    if (!output) {
        return E_POINTER;
    }
    *output = nullptr;
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    if (!g_real_direct3d_create9_ex) {
        return D3DERR_NOTAVAILABLE;
    }

    const D3DBackend requested_backend = RequestedBackend();
    D3DBackend effective_backend = D3DBackend::kNative9;
    bool fell_back = false;
    HRESULT result = D3DERR_NOTAVAILABLE;
    if (requested_backend == D3DBackend::kNineOnTwelve) {
        if (g_real_direct3d_create9_on12_ex) {
            D3D9ON12_ARGS arguments = {};
            arguments.Enable9On12 = TRUE;
            result = g_real_direct3d_create9_on12_ex(
                sdk_version,
                &arguments,
                1,
                output);
            if (SUCCEEDED(result) && *output) {
                effective_backend = D3DBackend::kNineOnTwelve;
            } else {
                fell_back = true;
                *output = nullptr;
            }
        } else {
            fell_back = true;
        }
    }
    if (!*output) {
        result = g_real_direct3d_create9_ex(sdk_version, output);
    }

    wchar_t message[320] = {};
    _snwprintf_s(
        message,
        _TRUNCATE,
        L"Direct3DCreate9Ex sdk=%u requested_backend=%ls "
        L"effective_backend=%ls fallback=%u result=0x%08lX object=%p",
        sdk_version,
        BackendName(requested_backend),
        BackendName(effective_backend),
        fell_back ? 1U : 0U,
        result,
        output ? *output : nullptr);
    interlude_modern::AppendLog(message);
    return result;
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    return g_real_d3dperf_begin_event
               ? g_real_d3dperf_begin_event(color, name)
               : -1;
}

extern "C" int WINAPI D3DPERF_EndEvent() {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    return g_real_d3dperf_end_event ? g_real_d3dperf_end_event() : -1;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus() {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    return g_real_d3dperf_get_status ? g_real_d3dperf_get_status() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    return g_real_d3dperf_query_repeat_frame
               ? g_real_d3dperf_query_repeat_frame()
               : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    if (g_real_d3dperf_set_marker) {
        g_real_d3dperf_set_marker(color, name);
    }
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    if (g_real_d3dperf_set_options) {
        g_real_d3dperf_set_options(options);
    }
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    if (g_real_d3dperf_set_region) {
        g_real_d3dperf_set_region(color, name);
    }
}

}  // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(unsigned int sdk_version) {
    InitOnceExecuteOnce(&g_init_once, LoadRealD3D9, nullptr, nullptr);
    if (!g_real_direct3d_create9) {
        return nullptr;
    }

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER start = {};
    LARGE_INTEGER end = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const D3DBackend requested_backend = RequestedBackend();
    D3DBackend effective_backend = D3DBackend::kNative9;
    bool fell_back = false;
    IDirect3D9* result = nullptr;

    if (requested_backend == D3DBackend::kNineOnTwelve) {
        if (g_real_direct3d_create9_on12) {
            D3D9ON12_ARGS arguments = {};
            arguments.Enable9On12 = TRUE;
            result = g_real_direct3d_create9_on12(
                sdk_version,
                &arguments,
                1);
            if (result) {
                effective_backend = D3DBackend::kNineOnTwelve;
            } else {
                fell_back = true;
                interlude_modern::AppendLog(
                    L"Direct3DCreate9On12 returned null; falling back to native9");
            }
        } else {
            fell_back = true;
            interlude_modern::AppendLog(
                L"Direct3DCreate9On12 unavailable; falling back to native9");
        }
    }
    if (!result) {
        result = g_real_direct3d_create9(sdk_version);
    }
    QueryPerformanceCounter(&end);

    wchar_t message[384] = {};
    const double elapsed_ms =
        frequency.QuadPart > 0
            ? (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) /
                  static_cast<double>(frequency.QuadPart)
            : 0.0;
    _snwprintf_s(
        message,
        _TRUNCATE,
        L"Direct3DCreate9 sdk=%u requested_backend=%ls "
        L"effective_backend=%ls fallback=%u result=%p elapsed_ms=%.3f",
        sdk_version,
        BackendName(requested_backend),
        BackendName(effective_backend),
        fell_back ? 1U : 0U,
        result,
        elapsed_ms);
    interlude_modern::AppendLog(message);
    return interlude_modern::WrapD3D9(result);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
