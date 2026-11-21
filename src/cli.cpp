#include "engine.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <string_view>

namespace {
std::atomic_bool interrupted{};
BOOL WINAPI on_signal(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true, std::memory_order_relaxed);
    return TRUE;
}
void print(DWORD stream, const std::wstring& text) {
    const auto handle = GetStdHandle(stream);
    DWORD mode{}, n{};
    if (GetConsoleMode(handle, &mode)) {
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
        return;
    }
    if (text.size() > INT_MAX) return;
    const int cap = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (cap <= 0) return;
    std::string utf8(cap, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), cap, nullptr, nullptr);
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &n, nullptr);
}
constexpr wchar_t usage[] = L"Usage: qcopy copy|move --to <directory> [--verify] "
    L"[--on-conflict skip|replace|rename] [--] <sources...>\n";
int invalid() { print(STD_ERROR_HANDLE, usage); return 2; }
}
int wmain(int count, wchar_t** argv) {
    using namespace std::literals;
    if (count == 2 && argv[1] == L"--help"sv) { print(STD_OUTPUT_HANDLE, usage); return 0; }
    if (count < 2) return invalid();
    qc::Job job;
    if (argv[1] == L"move"sv) job.operation = qc::Operation::move;
    else if (argv[1] != L"copy"sv) return invalid();
    bool positional = false;
    for (int i = 2; i < count; ++i) {
        const std::wstring_view arg(argv[i]);
        if (positional) job.sources.emplace_back(arg);
        else if (arg == L"--") positional = true;
        else if (arg == L"--verify") job.verify = true;
        else if (arg == L"--to" && i + 1 < count && job.destination.empty()) job.destination = argv[++i];
        else if (arg == L"--on-conflict" && i + 1 < count) {
            const std::wstring_view value(argv[++i]);
            if (value == L"skip") job.conflict = qc::Conflict::skip;
            else if (value == L"replace") job.conflict = qc::Conflict::replace;
            else if (value == L"rename") job.conflict = qc::Conflict::rename;
            else return invalid();
        } else if (arg == L"--benchmark-strategy" && i + 1 < count) {
            const std::wstring_view value(argv[++i]);
            if (value == L"native-single") job.strategy = qc::Strategy::native_single;
            else if (value == L"native-unbuffered") job.strategy = qc::Strategy::native_unbuffered;
            else if (value == L"experimental-iocp") job.strategy = qc::Strategy::experimental_iocp;
            else return invalid();
        } else if (!arg.starts_with(L"-")) job.sources.emplace_back(arg);
        else return invalid();
    }
    if (job.destination.empty() || job.sources.empty()) return invalid();
    SetConsoleCtrlHandler(on_signal, TRUE);
    qc::Progress progress;
    const auto result = qc::run(job, progress, interrupted, [](const qc::Issue& issue) {
        print(STD_ERROR_HANDLE, std::wstring(issue.skipped ? L"Skipped: " : L"Failed: ") + issue.path + L" — " +
              issue.stage + L" (" + std::to_wstring(issue.code) + L"): " + qc::error_message(issue.code) + L"\n");
    });
    print(STD_OUTPUT_HANDLE, (result.cancelled ? std::wstring(L"Cancelled. ") : std::wstring()) +
        std::to_wstring(progress.completed.load()) + L" item(s) processed, " +
        std::to_wstring(progress.transferred.load()) + L" transferred bytes, " +
        std::to_wstring(result.failed) + L" failed, " + std::to_wstring(result.skipped) + L" skipped.\n");
    if (job.strategy != qc::Strategy::automatic)
        print(STD_OUTPUT_HANDLE, L"IOCP=" + std::to_wstring(progress.iocp_completed.load()) +
            L" unbuffered=" + std::to_wstring(progress.unbuffered_completed.load()) + L"\n");
    return result.exit_code();
}
