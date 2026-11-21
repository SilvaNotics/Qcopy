#include "win.h"

namespace qc::win {
void inject(std::wstring_view phase, const std::wstring& source) {
    wchar_t mode[32]{};
    if (!GetEnvironmentVariableW(L"QCOPY_TEST_FAULT", mode, static_cast<DWORD>(std::size(mode)))) return;
    const std::wstring_view fault(mode);
    if (phase == L"write" && fault == L"disk-full") fail(L"Write target", ERROR_DISK_FULL);
    if (phase == L"write" && fault == L"write-fault") fail(L"Write target", ERROR_WRITE_FAULT);
    if (phase == L"write" && fault == L"wait-ctrl-break") Sleep(300);
    if (phase == L"blocks-pending" && fault == L"fail-iocp") fail(L"Write target", ERROR_WRITE_FAULT);
    if ((phase == L"before-publish" && fault == L"crash-before") ||
        (phase == L"after-publish" && fault == L"crash-after")) TerminateProcess(GetCurrentProcess(), 91);
    if (phase == L"after-publish" && fault == L"replace-source") {
        const auto old = source + L".old";
        check(MoveFileW(source.c_str(), old.c_str()), L"Replace source in fixture");
        const auto replacement = open(source, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW);
        DWORD written{};
        check(WriteFile(replacement.get(), "new source", 10, &written, nullptr), L"Create replacement fixture");
    }
}
}
