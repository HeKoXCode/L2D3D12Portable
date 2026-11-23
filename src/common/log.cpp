#include "common/log.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace interlude_modern {
namespace {

SRWLOCK g_log_lock = SRWLOCK_INIT;

std::wstring LogDirectory() {
    wchar_t configured[32768] = {};
    const DWORD configured_length = GetEnvironmentVariableW(
        L"L2D3D12_LOG_DIR",
        configured,
        static_cast<DWORD>(std::size(configured)));
    if (configured_length > 0 && configured_length < std::size(configured)) {
        const DWORD attributes = GetFileAttributesW(configured);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return configured;
        }
    }

    wchar_t local_app_data[MAX_PATH] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }

    std::wstring directory(local_app_data);
    directory += L"\\L2D3D12Portable";
    CreateDirectoryW(directory.c_str(), nullptr);
    directory += L"\\logs";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

}  // namespace

void AppendLog(const wchar_t* message) {
    OutputDebugStringW(message);

    const std::wstring directory = LogDirectory();
    if (directory.empty()) {
        return;
    }

    AcquireSRWLockExclusive(&g_log_lock);
    const std::wstring log_path = directory + L"\\d3d9-proxy.log";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, log_path.c_str(), L"a+, ccs=UTF-8") == 0 && stream) {
        SYSTEMTIME utc = {};
        GetSystemTime(&utc);
        fwprintf(
            stream,
            L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu tid=%lu %ls\n",
            utc.wYear,
            utc.wMonth,
            utc.wDay,
            utc.wHour,
            utc.wMinute,
            utc.wSecond,
            utc.wMilliseconds,
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            message);
        fclose(stream);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void AppendTelemetry(
    std::uint64_t first_frame,
    std::uint64_t last_frame,
    double average_frame_ms,
    double p50_frame_ms,
    double p95_frame_ms,
    double p99_frame_ms,
    double maximum_frame_ms,
    double average_present_ms,
    long present_result) {
    const std::wstring directory = LogDirectory();
    if (directory.empty()) {
        return;
    }

    AcquireSRWLockExclusive(&g_log_lock);
    const std::wstring csv_path = directory + L"\\frame-telemetry.csv";
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    const bool has_existing_data =
        GetFileAttributesExW(csv_path.c_str(), GetFileExInfoStandard, &attributes) &&
        (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0);

    FILE* stream = nullptr;
    if (_wfopen_s(&stream, csv_path.c_str(), L"a+, ccs=UTF-8") == 0 && stream) {
        if (!has_existing_data) {
            fwprintf(
                stream,
                L"utc,pid,first_frame,last_frame,average_frame_ms,p50_frame_ms,"
                L"p95_frame_ms,p99_frame_ms,maximum_frame_ms,average_present_ms,"
                L"present_hresult\n");
        }

        SYSTEMTIME utc = {};
        GetSystemTime(&utc);
        fwprintf(
            stream,
            L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ,%lu,%llu,%llu,"
            L"%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,0x%08lX\n",
            utc.wYear,
            utc.wMonth,
            utc.wDay,
            utc.wHour,
            utc.wMinute,
            utc.wSecond,
            utc.wMilliseconds,
            GetCurrentProcessId(),
            static_cast<unsigned long long>(first_frame),
            static_cast<unsigned long long>(last_frame),
            average_frame_ms,
            p50_frame_ms,
            p95_frame_ms,
            p99_frame_ms,
            maximum_frame_ms,
            average_present_ms,
            present_result);
        fclose(stream);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

}  // namespace interlude_modern
