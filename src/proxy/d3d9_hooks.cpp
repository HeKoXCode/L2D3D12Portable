#include "proxy/d3d9_hooks.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>

#include "common/log.h"

namespace interlude_modern {
namespace {

constexpr std::size_t kDeviceResetIndex = 16;
constexpr std::size_t kDevicePresentIndex = 17;
constexpr std::size_t kTelemetryBatchSize = 600;

using ResetFn =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*,
    const RECT*,
    const RECT*,
    HWND,
    const RGNDATA*);

ResetFn g_real_reset = nullptr;
PresentFn g_real_present = nullptr;
SRWLOCK g_hook_lock = SRWLOCK_INIT;
SRWLOCK g_telemetry_lock = SRWLOCK_INIT;

std::array<double, kTelemetryBatchSize> g_frame_ms = {};
std::array<double, kTelemetryBatchSize> g_present_ms = {};
std::size_t g_sample_count = 0;
std::uint64_t g_total_frames = 0;
std::int64_t g_last_present_entry = 0;
std::int64_t g_qpc_frequency = 0;
bool g_skip_next_interval = true;
bool g_reported_first_present = false;

bool TelemetryHooksEnabled() {
    wchar_t value[16] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"L2D3D12_TELEMETRY",
        value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return true;
    }
    return _wcsicmp(value, L"0") != 0 &&
           _wcsicmp(value, L"off") != 0 &&
           _wcsicmp(value, L"false") != 0;
}

void* FunctionAddress(ResetFn function) {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(function));
}

void* FunctionAddress(PresentFn function) {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(function));
}

template <typename Function>
Function FunctionFromAddress(void* address) {
    return reinterpret_cast<Function>(reinterpret_cast<std::uintptr_t>(address));
}

bool PatchVtableEntry(
    void** vtable,
    std::size_t index,
    void* replacement,
    void** original) {
    if (vtable[index] == replacement) {
        return true;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            &vtable[index],
            sizeof(void*),
            PAGE_READWRITE,
            &old_protection)) {
        return false;
    }

    *original = InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&vtable[index]),
        replacement);

    DWORD ignored_protection = 0;
    const BOOL restore_result = VirtualProtect(
        &vtable[index],
        sizeof(void*),
        old_protection,
        &ignored_protection);
    return restore_result != FALSE;
}

double Milliseconds(std::int64_t ticks) {
    return g_qpc_frequency > 0
               ? (static_cast<double>(ticks) * 1000.0) /
                     static_cast<double>(g_qpc_frequency)
               : 0.0;
}

double Percentile(
    const std::array<double, kTelemetryBatchSize>& sorted,
    double percentile) {
    const double position =
        percentile * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper =
        (std::min)(lower + 1, static_cast<std::size_t>(sorted.size() - 1));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

void ResetTelemetryState() {
    AcquireSRWLockExclusive(&g_telemetry_lock);
    g_sample_count = 0;
    g_last_present_entry = 0;
    g_skip_next_interval = true;
    ReleaseSRWLockExclusive(&g_telemetry_lock);
}

void RecordPresent(
    std::int64_t entry_ticks,
    std::int64_t exit_ticks,
    HRESULT present_result) {
    std::array<double, kTelemetryBatchSize> frame_snapshot = {};
    std::array<double, kTelemetryBatchSize> present_snapshot = {};
    std::uint64_t first_frame = 0;
    std::uint64_t last_frame = 0;
    bool emit_summary = false;
    bool report_first_present = false;

    AcquireSRWLockExclusive(&g_telemetry_lock);
    g_total_frames += 1;
    if (!g_reported_first_present) {
        g_reported_first_present = true;
        report_first_present = true;
    }

    if (g_skip_next_interval) {
        g_skip_next_interval = false;
        g_last_present_entry = entry_ticks;
    } else if (g_last_present_entry != 0) {
        g_frame_ms[g_sample_count] =
            Milliseconds(entry_ticks - g_last_present_entry);
        g_present_ms[g_sample_count] =
            Milliseconds(exit_ticks - entry_ticks);
        g_sample_count += 1;
        g_last_present_entry = entry_ticks;

        if (g_sample_count == kTelemetryBatchSize) {
            frame_snapshot = g_frame_ms;
            present_snapshot = g_present_ms;
            last_frame = g_total_frames;
            first_frame = last_frame - kTelemetryBatchSize + 1;
            g_sample_count = 0;
            g_skip_next_interval = true;
            emit_summary = true;
        }
    } else {
        g_last_present_entry = entry_ticks;
    }
    ReleaseSRWLockExclusive(&g_telemetry_lock);

    if (report_first_present) {
        AppendLog(L"First Present observed; frame telemetry is active");
    }

    if (!emit_summary) {
        return;
    }

    double frame_sum = 0.0;
    double present_sum = 0.0;
    for (std::size_t index = 0; index < kTelemetryBatchSize; ++index) {
        frame_sum += frame_snapshot[index];
        present_sum += present_snapshot[index];
    }
    std::sort(frame_snapshot.begin(), frame_snapshot.end());

    const double average_frame =
        frame_sum / static_cast<double>(kTelemetryBatchSize);
    const double average_present =
        present_sum / static_cast<double>(kTelemetryBatchSize);
    const double p50 = Percentile(frame_snapshot, 0.50);
    const double p95 = Percentile(frame_snapshot, 0.95);
    const double p99 = Percentile(frame_snapshot, 0.99);
    const double maximum = frame_snapshot.back();

    AppendTelemetry(
        first_frame,
        last_frame,
        average_frame,
        p50,
        p95,
        p99,
        maximum,
        average_present,
        present_result);

    wchar_t message[384] = {};
    _snwprintf_s(
        message,
        _TRUNCATE,
        L"Frame batch %llu-%llu avg=%.3fms p50=%.3fms p95=%.3fms "
        L"p99=%.3fms max=%.3fms present_avg=%.3fms",
        static_cast<unsigned long long>(first_frame),
        static_cast<unsigned long long>(last_frame),
        average_frame,
        p50,
        p95,
        p99,
        maximum,
        average_present);
    AppendLog(message);
}

HRESULT STDMETHODCALLTYPE HookReset(
    IDirect3DDevice9* device,
    D3DPRESENT_PARAMETERS* parameters) {
    AppendLog(L"IDirect3DDevice9::Reset begin");
    ResetTelemetryState();
    const HRESULT result = g_real_reset(device, parameters);

    wchar_t message[128] = {};
    _snwprintf_s(
        message,
        _TRUNCATE,
        L"IDirect3DDevice9::Reset end result=0x%08lX",
        result);
    AppendLog(message);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent(
    IDirect3DDevice9* device,
    const RECT* source_rect,
    const RECT* destination_rect,
    HWND destination_window,
    const RGNDATA* dirty_region) {
    LARGE_INTEGER entry = {};
    LARGE_INTEGER exit = {};
    QueryPerformanceCounter(&entry);
    const HRESULT result = g_real_present(
        device,
        source_rect,
        destination_rect,
        destination_window,
        dirty_region);
    QueryPerformanceCounter(&exit);
    RecordPresent(entry.QuadPart, exit.QuadPart, result);
    return result;
}

bool HookDevice(IDirect3DDevice9* device) {
    if (!device) {
        return false;
    }

    AcquireSRWLockExclusive(&g_hook_lock);
    void*** object_vtable = reinterpret_cast<void***>(device);
    void** current_vtable = *object_vtable;
    if (current_vtable[kDevicePresentIndex] == FunctionAddress(&HookPresent)) {
        ReleaseSRWLockExclusive(&g_hook_lock);
        return true;
    }

    void* real_reset = nullptr;
    if (!PatchVtableEntry(
            current_vtable,
            kDeviceResetIndex,
            FunctionAddress(&HookReset),
            &real_reset)) {
        ReleaseSRWLockExclusive(&g_hook_lock);
        return false;
    }
    g_real_reset = FunctionFromAddress<ResetFn>(real_reset);

    void* real_present = nullptr;
    if (!PatchVtableEntry(
            current_vtable,
            kDevicePresentIndex,
            FunctionAddress(&HookPresent),
            &real_present)) {
        void* ignored = nullptr;
        PatchVtableEntry(
            current_vtable,
            kDeviceResetIndex,
            real_reset,
            &ignored);
        g_real_reset = nullptr;
        ReleaseSRWLockExclusive(&g_hook_lock);
        return false;
    }
    g_real_present = FunctionFromAddress<PresentFn>(real_present);

    wchar_t addresses[320] = {};
    _snwprintf_s(
        addresses,
        _TRUNCATE,
        L"Device vtable=%p reset_real=%p reset_hook=%p "
        L"present_real=%p present_hook=%p clear=%p",
        current_vtable,
        FunctionAddress(g_real_reset),
        FunctionAddress(&HookReset),
        FunctionAddress(g_real_present),
        FunctionAddress(&HookPresent),
        current_vtable[43]);
    AppendLog(addresses);

    ReleaseSRWLockExclusive(&g_hook_lock);

    AppendLog(L"IDirect3DDevice9 Reset/Present telemetry hooks installed");
    return true;
}

HRESULT CreateDeviceWithTelemetry(
    IDirect3D9* d3d9,
    UINT adapter,
    D3DDEVTYPE device_type,
    HWND focus_window,
    DWORD behavior_flags,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** returned_device) {
    wchar_t begin_message[320] = {};
    _snwprintf_s(
        begin_message,
        _TRUNCATE,
        L"IDirect3D9::CreateDevice adapter=%u type=%u behavior=0x%08lX "
        L"windowed=%u backbuffer=%ux%u interval=0x%08X",
        adapter,
        static_cast<unsigned int>(device_type),
        behavior_flags,
        parameters ? parameters->Windowed : 0,
        parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0,
        parameters ? parameters->PresentationInterval : 0);
    AppendLog(begin_message);

    const HRESULT result = d3d9->CreateDevice(
        adapter,
        device_type,
        focus_window,
        behavior_flags,
        parameters,
        returned_device);

    if (SUCCEEDED(result) && returned_device && *returned_device) {
        IDirect3DDevice9On12* device_on_12 = nullptr;
        const HRESULT backend_query = (*returned_device)->QueryInterface(
            __uuidof(IDirect3DDevice9On12),
            reinterpret_cast<void**>(&device_on_12));
        if (SUCCEEDED(backend_query) && device_on_12) {
            AppendLog(L"CreateDevice effective_backend=9on12");
            ID3D12Device* d3d12_device = nullptr;
            const HRESULT device_query = device_on_12->GetD3D12Device(
                __uuidof(ID3D12Device),
                reinterpret_cast<void**>(&d3d12_device));
            if (SUCCEEDED(device_query) && d3d12_device) {
                const LUID adapter_luid = d3d12_device->GetAdapterLuid();
                wchar_t device_message[224] = {};
                _snwprintf_s(
                    device_message,
                    _TRUNCATE,
                    L"D3D9On12 device adapter_luid=%08lX:%08lX nodes=%u",
                    static_cast<unsigned long>(adapter_luid.HighPart),
                    static_cast<unsigned long>(adapter_luid.LowPart),
                    d3d12_device->GetNodeCount());
                AppendLog(device_message);
                d3d12_device->Release();
            } else {
                AppendLog(L"D3D9On12 GetD3D12Device failed");
            }
            device_on_12->Release();
        } else {
            AppendLog(L"CreateDevice effective_backend=native9");
        }
        if (TelemetryHooksEnabled()) {
            if (!HookDevice(*returned_device)) {
                AppendLog(
                    L"CreateDevice succeeded but device telemetry hook failed");
            }
        } else {
            AppendLog(
                L"Device telemetry hooks disabled by L2D3D12_TELEMETRY");
        }
    }

    wchar_t end_message[160] = {};
    _snwprintf_s(
        end_message,
        _TRUNCATE,
        L"IDirect3D9::CreateDevice result=0x%08lX device=%p",
        result,
        returned_device ? *returned_device : nullptr);
    AppendLog(end_message);
    return result;
}

class D3D9Proxy final : public IDirect3D9 {
public:
    explicit D3D9Proxy(IDirect3D9* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (InlineIsEqualGUID(interface_id, __uuidof(IUnknown)) ||
            InlineIsEqualGUID(interface_id, __uuidof(IDirect3D9))) {
            *object = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(interface_id, object);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        real_->AddRef();
        return static_cast<ULONG>(InterlockedIncrement(&reference_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        real_->Release();
        const LONG remaining = InterlockedDecrement(&reference_count_);
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(
        void* initialize_function) override {
        return real_->RegisterSoftwareDevice(initialize_function);
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return real_->GetAdapterCount();
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(
        UINT adapter,
        DWORD flags,
        D3DADAPTER_IDENTIFIER9* identifier) override {
        return real_->GetAdapterIdentifier(adapter, flags, identifier);
    }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(
        UINT adapter,
        D3DFORMAT format) override {
        return real_->GetAdapterModeCount(adapter, format);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(
        UINT adapter,
        D3DFORMAT format,
        UINT mode,
        D3DDISPLAYMODE* display_mode) override {
        return real_->EnumAdapterModes(adapter, format, mode, display_mode);
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(
        UINT adapter,
        D3DDISPLAYMODE* display_mode) override {
        return real_->GetAdapterDisplayMode(adapter, display_mode);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DFORMAT adapter_format,
        D3DFORMAT back_buffer_format,
        BOOL windowed) override {
        return real_->CheckDeviceType(
            adapter,
            device_type,
            adapter_format,
            back_buffer_format,
            windowed);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DFORMAT adapter_format,
        DWORD usage,
        D3DRESOURCETYPE resource_type,
        D3DFORMAT check_format) override {
        return real_->CheckDeviceFormat(
            adapter,
            device_type,
            adapter_format,
            usage,
            resource_type,
            check_format);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DFORMAT surface_format,
        BOOL windowed,
        D3DMULTISAMPLE_TYPE multi_sample_type,
        DWORD* quality_levels) override {
        return real_->CheckDeviceMultiSampleType(
            adapter,
            device_type,
            surface_format,
            windowed,
            multi_sample_type,
            quality_levels);
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DFORMAT adapter_format,
        D3DFORMAT render_target_format,
        D3DFORMAT depth_stencil_format) override {
        return real_->CheckDepthStencilMatch(
            adapter,
            device_type,
            adapter_format,
            render_target_format,
            depth_stencil_format);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DFORMAT source_format,
        D3DFORMAT target_format) override {
        return real_->CheckDeviceFormatConversion(
            adapter,
            device_type,
            source_format,
            target_format);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(
        UINT adapter,
        D3DDEVTYPE device_type,
        D3DCAPS9* capabilities) override {
        return real_->GetDeviceCaps(adapter, device_type, capabilities);
    }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
        return real_->GetAdapterMonitor(adapter);
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT adapter,
        D3DDEVTYPE device_type,
        HWND focus_window,
        DWORD behavior_flags,
        D3DPRESENT_PARAMETERS* parameters,
        IDirect3DDevice9** returned_device) override {
        return CreateDeviceWithTelemetry(
            real_,
            adapter,
            device_type,
            focus_window,
            behavior_flags,
            parameters,
            returned_device);
    }

private:
    ~D3D9Proxy() = default;

    IDirect3D9* real_;
    volatile LONG reference_count_ = 1;
};

}  // namespace

IDirect3D9* WrapD3D9(IDirect3D9* d3d9) {
    if (!d3d9) {
        return nullptr;
    }

    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    g_qpc_frequency = frequency.QuadPart;
    D3D9Proxy* wrapper = new (std::nothrow) D3D9Proxy(d3d9);
    if (!wrapper) {
        AppendLog(L"IDirect3D9 wrapper allocation failed; using real interface");
        return d3d9;
    }
    AppendLog(L"IDirect3D9 telemetry wrapper installed");
    return wrapper;
}

}  // namespace interlude_modern
