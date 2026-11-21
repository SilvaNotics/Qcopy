#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <memory>
#include <cstdio>

namespace {
template<class T> struct Release { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, Release<T>>;
HRESULT shell_copy(const wchar_t* source, const wchar_t* target) {
    IFileOperation* raw_ops = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_ops));
    if (FAILED(hr)) return hr;
    const ComPtr<IFileOperation> ops(raw_ops);
    IShellItem* raw_input = nullptr;
    hr = SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&raw_input));
    if (FAILED(hr)) return hr;
    const ComPtr<IShellItem> input(raw_input);
    IShellItem* raw_folder = nullptr;
    hr = SHCreateItemFromParsingName(target, nullptr, IID_PPV_ARGS(&raw_folder));
    if (FAILED(hr)) return hr;
    const ComPtr<IShellItem> folder(raw_folder);
    hr = ops->SetOperationFlags(FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR);
    if (FAILED(hr)) return hr;
    hr = ops->CopyItem(input.get(), folder.get(), nullptr, nullptr);
    if (FAILED(hr)) return hr;
    hr = ops->PerformOperations();
    if (FAILED(hr)) return hr;
    BOOL aborted = FALSE;
    hr = ops->GetAnyOperationsAborted(&aborted);
    return SUCCEEDED(hr) && aborted ? E_ABORT : hr;
}
}
int wmain(int count, wchar_t** args) {
    if (count != 3) return 2;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    const auto hr = shell_copy(args[1], args[2]);
    CoUninitialize();
    if (FAILED(hr)) std::fwprintf(stderr, L"Shell copy failed: 0x%08lx\n", static_cast<unsigned long>(hr));
    return FAILED(hr) ? 1 : 0;
}
