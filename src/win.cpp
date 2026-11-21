#include "win.h"
#include <bcrypt.h>
#include <winioctl.h>
#include <winternl.h>
#include <array>
#include <cstddef>
#include <cstring>

namespace qc {
std::wstring error_message(unsigned long code) {
    wchar_t* raw = nullptr;
    const auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring result = n ? std::wstring(raw, n) : L"Windows error " + std::to_wstring(code);
    if (raw) LocalFree(raw);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();
    return result;
}
std::wstring display_path(const std::wstring& p) {
    if (p.starts_with(L"\\\\?\\") && p.size() >= 7 && p[5] == L':') return p.substr(4);
    if (p.starts_with(L"\\\\?\\Volume{")) {
        const auto end = p.find(L'\\', 4);
        if (end != std::wstring::npos) {
            wchar_t paths[1024]{};
            DWORD used{};
            if (GetVolumePathNamesForVolumeNameW(p.substr(0, end + 1).c_str(), paths, static_cast<DWORD>(std::size(paths)), &used) && *paths)
                return paths + p.substr(end + 1);
        }
    }
    return p;
}
}

namespace qc::win {
Handle open(const std::wstring& p, DWORD access, DWORD share, DWORD flags, DWORD disposition) {
    Handle result(CreateFileW(p.c_str(), access, share, nullptr, disposition,
        flags | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!result) fail(L"Open");
    return result;
}
std::wstring absolute(const std::wstring& input) {
    if (input.empty() || input.starts_with(L"\\\\.\\") || input.starts_with(L"\\??\\"))
        fail(L"Invalid local path", ERROR_BAD_PATHNAME);
    std::wstring p = input;
    if (!p.starts_with(L"\\\\?\\")) {
        const DWORD cap = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
        if (!cap) fail(L"Resolve path");
        std::wstring full(cap, L'\0');
        const DWORD n = GetFullPathNameW(p.c_str(), cap, full.data(), nullptr);
        if (!n || n >= cap) fail(L"Resolve path", ERROR_BAD_PATHNAME);
        full.resize(n);
        p = L"\\\\?\\" + full;
    }
    if (p.size() < 7 || !((p[4] >= L'A' && p[4] <= L'Z') || (p[4] >= L'a' && p[4] <= L'z')) ||
        p[5] != L':' || p[6] != L'\\' || p.find_first_of(L":/*?", 7) != std::wstring::npos)
        fail(L"Only local drive paths are supported", ERROR_BAD_PATHNAME);
    while (p.size() > 7 && p.back() == L'\\') p.pop_back();
    for (size_t start = 7; start < p.size();) {
        const auto end = p.find(L'\\', start);
        const auto name = std::wstring_view(p).substr(start, end == std::wstring::npos ? end : end - start);
        if (name.empty() || name == L"." || name == L"..") fail(L"Invalid path segment", ERROR_BAD_PATHNAME);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (GetDriveTypeW(p.substr(4, 3).c_str()) == DRIVE_REMOTE) fail(L"Only local drives are supported", ERROR_NOT_SUPPORTED);
    return p;
}
std::wstring final_path(const Handle& h) {
    for (DWORD style : {DWORD(VOLUME_NAME_GUID), DWORD(VOLUME_NAME_DOS)}) {
        const DWORD cap = GetFinalPathNameByHandleW(h.get(), nullptr, 0, style);
        if (!cap) continue;
        std::wstring p(cap, L'\0');
        const DWORD n = GetFinalPathNameByHandleW(h.get(), p.data(), cap, style);
        if (n && n < cap) { p.resize(n); return p; }
    }
    fail(L"Resolve file handle path");
}
std::wstring leaf(const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); }
std::wstring join(const std::wstring& p, std::wstring_view name) {
    auto result = p;
    if (!result.ends_with(L'\\')) result += L'\\';
    result += name;
    return result;
}
bool contains(const std::wstring& parent, const std::wstring& child) {
    if (child.size() < parent.size() || CompareStringOrdinal(parent.data(), static_cast<int>(parent.size()),
        child.data(), static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL) return false;
    return child.size() == parent.size() || parent.ends_with(L'\\') || child[parent.size()] == L'\\';
}
Info::Info(const Handle& h, bool detailed) {
    check(GetFileInformationByHandle(h.get(), &id), L"Read file identity");
    basic.FileAttributes = id.dwFileAttributes;
    if (!detailed) return;
    check(GetFileInformationByHandleEx(h.get(), FileBasicInfo, &basic, sizeof basic), L"Read file attributes");
    has_extended_id = GetFileInformationByHandleEx(h.get(), FileIdInfo, &extended_id, sizeof extended_id) != FALSE;
}
uint64_t Info::size() const { return (uint64_t{id.nFileSizeHigh} << 32) | id.nFileSizeLow; }
bool Info::same(const Info& other) const {
    if (has_extended_id && other.has_extended_id) {
        return extended_id.VolumeSerialNumber == other.extended_id.VolumeSerialNumber &&
            std::memcmp(&extended_id.FileId, &other.extended_id.FileId, sizeof(FILE_ID_128)) == 0;
    }
    return id.dwVolumeSerialNumber == other.id.dwVolumeSerialNumber &&
        id.nFileIndexHigh == other.id.nFileIndexHigh && id.nFileIndexLow == other.id.nFileIndexLow;
}
bool Info::unchanged(const Info& other) const {
    return same(other) && size() == other.size() && basic.ChangeTime.QuadPart == other.basic.ChangeTime.QuadPart &&
        basic.LastWriteTime.QuadPart == other.basic.LastWriteTime.QuadPart && basic.CreationTime.QuadPart == other.basic.CreationTime.QuadPart;
}
Volume volume(const std::wstring& p) {
    std::vector<wchar_t> root(32768);
    check(GetVolumePathNameW(p.c_str(), root.data(), static_cast<DWORD>(root.size())), L"Find volume root");
    Volume result;
    wchar_t fs[MAX_PATH]{};
    check(GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr, &result.flags, fs, MAX_PATH), L"Query volume");
    result.ntfs = _wcsicmp(fs, L"NTFS") == 0;
    wchar_t guid[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.data(), guid, MAX_PATH)) {
        if (!std::wstring_view(root.data()).starts_with(L"\\\\?\\Volume{")) return result;
        wcscpy_s(guid, root.data());
    }
    const size_t length = wcslen(guid);
    if (!length) return result;
    if (guid[length - 1] == L'\\') guid[length - 1] = L'\0';
    Handle vol(CreateFileW(guid, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    VOLUME_DISK_EXTENTS extents{};
    DWORD used{};
    if (!vol || !DeviceIoControl(vol.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
        &extents, sizeof extents, &used, nullptr) || extents.NumberOfDiskExtents != 1) return result;
    result.disk = extents.Extents[0].DiskNumber;
    const auto physical = L"\\\\.\\PhysicalDrive" + std::to_wstring(result.disk);
    Handle disk(CreateFileW(physical.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!disk) return result;
    STORAGE_PROPERTY_QUERY query{StorageDeviceProperty, PropertyStandardQuery, {}};
    alignas(STORAGE_DEVICE_DESCRIPTOR) std::array<std::byte, 1024> data{};
    if (!DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query,
        data.data(), static_cast<DWORD>(data.size()), &used, nullptr) || used < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return result;
    const auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(data.data())->BusType;
    if (bus != BusTypeNvme && bus != BusTypeSata && bus != BusTypeAta && bus != BusTypeSas) return result;
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    result.internal_ssd = DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query,
        &seek, sizeof seek, &used, nullptr) && used >= sizeof seek && !seek.IncursSeekPenalty;
    return result;
}
void rename(const Handle& h, const std::wstring& to, bool replace) {
    const auto bytes = static_cast<DWORD>(to.size() * sizeof(wchar_t));
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = replace;
    info->FileNameLength = bytes;
    std::memcpy(info->FileName, to.data(), bytes);
    check(SetFileInformationByHandle(h.get(), FileRenameInfo, info, static_cast<DWORD>(storage.size())), L"Publish by rename");
}
void remove(const Handle& h, bool ignore_readonly) {
    if (ignore_readonly) {
        FILE_DISPOSITION_INFO_EX info{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
        if (SetFileInformationByHandle(h.get(), FileDispositionInfoEx, &info, sizeof info)) return;
    }
    FILE_DISPOSITION_INFO info{TRUE};
    check(SetFileInformationByHandle(h.get(), FileDispositionInfo, &info, sizeof info), L"Delete by handle");
}
bool missing(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }
std::vector<std::wstring> streams(const Handle& h, const Volume& vol) {
    std::vector<std::wstring> result;
    if (!(vol.flags & FILE_NAMED_STREAMS)) return result;
    std::vector<std::byte> buffer(1024);
    while (!GetFileInformationByHandleEx(h.get(), FileStreamInfo, buffer.data(), static_cast<DWORD>(buffer.size()))) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF) return result;
        if (code != ERROR_MORE_DATA && code != ERROR_INSUFFICIENT_BUFFER) fail(L"List data streams", code);
        if (buffer.size() >= 16 * 1024 * 1024) fail(L"List data streams", ERROR_BUFFER_OVERFLOW);
        buffer.resize(buffer.size() * 2);
    }
    size_t offset = 0;
    for (;;) {
        const auto available = buffer.size() - offset;
        if (available < offsetof(FILE_STREAM_INFO, StreamName)) fail(L"List data streams", ERROR_INVALID_DATA);
        const auto* info = reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data() + offset);
        if (info->StreamNameLength > available - offsetof(FILE_STREAM_INFO, StreamName) || info->StreamNameLength % sizeof(wchar_t))
            fail(L"List data streams", ERROR_INVALID_DATA);
        std::wstring name(info->StreamName, info->StreamNameLength / sizeof(wchar_t));
        if (name != L"::$DATA") result.push_back(std::move(name));
        if (!info->NextEntryOffset) return result;
        if (info->NextEntryOffset > available || info->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName))
            fail(L"List data streams", ERROR_INVALID_DATA);
        offset += info->NextEntryOffset;
    }
}
namespace {
template<typename Fn> Fn ntdll_function(const char* name) {
    const auto proc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name);
    Fn fn = nullptr;
    static_assert(sizeof proc == sizeof fn);
    std::memcpy(&fn, &proc, sizeof fn);
    return fn;
}
}
bool has_eas(const Handle& h) {
    using Query = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    static const auto query = ntdll_function<Query>("NtQueryInformationFile");
    IO_STATUS_BLOCK status{};
    ULONG size{};
    constexpr auto file_ea_information = static_cast<FILE_INFORMATION_CLASS>(7);
    if (!query || query(h.get(), &status, &size, sizeof size, file_ea_information) < 0)
        fail(L"Query extended attributes", ERROR_NOT_SUPPORTED);
    return size != 0;
}
std::vector<std::byte> read_eas(const Handle& h) {
    if (!has_eas(h)) return {};
    using Query = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PULONG, BOOLEAN);
    static const auto query = ntdll_function<Query>("NtQueryEaFile");
    if (!query) fail(L"Read extended attributes", ERROR_NOT_SUPPORTED);
    std::vector<std::byte> buffer(64 * 1024);
    IO_STATUS_BLOCK status{};
    if (query(h.get(), &status, buffer.data(), static_cast<ULONG>(buffer.size()), FALSE, nullptr, 0, nullptr, TRUE) < 0 ||
        status.Information > buffer.size()) fail(L"Read extended attributes", ERROR_EA_LIST_INCONSISTENT);
    buffer.resize(status.Information);
    return buffer;
}
void write_eas(const Handle& h, std::vector<std::byte>& eas) {
    if (eas.empty()) return;
    using Set = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG);
    static const auto set = ntdll_function<Set>("NtSetEaFile");
    IO_STATUS_BLOCK status{};
    if (!set || set(h.get(), &status, eas.data(), static_cast<ULONG>(eas.size())) < 0)
        fail(L"Write extended attributes", ERROR_EAS_NOT_SUPPORTED);
}
std::wstring temporary(const std::wstring& directory) {
    std::array<UCHAR, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        fail(L"Generate a temporary name", ERROR_GEN_FAILURE);
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L".qcopy-";
    for (auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    return join(directory, name + L".part");
}
}
