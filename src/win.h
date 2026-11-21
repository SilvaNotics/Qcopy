#pragma once
#include "engine.h"
#include <windows.h>
#include <cstddef>
#include <string_view>
#include <utility>

namespace qc::win {
struct Error { DWORD code; const wchar_t* stage; };
[[noreturn]] inline void fail(const wchar_t* stage, DWORD code = GetLastError()) { throw Error{code, stage}; }
inline void check(BOOL ok, const wchar_t* stage) { if (!ok) fail(stage); }
inline void cancelled(const std::atomic_bool& stop) {
    if (stop.load(std::memory_order_relaxed)) fail(L"Cancelled", ERROR_CANCELLED);
}
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE v) : value_(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value_(std::exchange(h.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& h) noexcept {
        if (this != &h) { reset(); value_ = std::exchange(h.value_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    void reset() noexcept {
        if (*this) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
};
Handle open(const std::wstring&, DWORD access, DWORD share, DWORD flags = 0, DWORD disposition = OPEN_EXISTING);
std::wstring absolute(const std::wstring&);
std::wstring final_path(const Handle&);
std::wstring leaf(const std::wstring&);
std::wstring join(const std::wstring&, std::wstring_view);
bool contains(const std::wstring& parent, const std::wstring& child);
struct Info {
    BY_HANDLE_FILE_INFORMATION id{};
    FILE_BASIC_INFO basic{};
    bool has_extended_id{};
    FILE_ID_INFO extended_id{};
    explicit Info(const Handle&, bool detailed = true);
    bool dir() const { return (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool link() const { return (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
    uint64_t size() const;
    bool same(const Info&) const;
    bool unchanged(const Info&) const;
};
struct Volume {
    DWORD flags{};
    bool ntfs{};
    bool internal_ssd{};
    DWORD disk = MAXDWORD;
};
Volume volume(const std::wstring&);
void rename(const Handle&, const std::wstring& to, bool replace);
void remove(const Handle&, bool ignore_readonly = false);
std::vector<std::wstring> streams(const Handle&, const Volume&);
bool has_eas(const Handle&);
std::vector<std::byte> read_eas(const Handle&);
void write_eas(const Handle&, std::vector<std::byte>&);
std::wstring temporary(const std::wstring& directory);
bool missing(DWORD);
}
