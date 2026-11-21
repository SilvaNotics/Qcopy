#include "transfer.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

namespace qc::win {
#ifdef QCOPY_INSTRUMENTED
void inject(std::wstring_view phase, const std::wstring& source);
#endif
namespace {
constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr uint64_t large = 512ull * 1024 * 1024;

class Temporary {
public:
    std::wstring path;
    bool owned = false;
    explicit Temporary(const std::wstring& parent) : path(temporary(parent)) {}
    ~Temporary() { cleanup(); }
    void cleanup() {
        if (!owned) return;
        try {
            auto file = open(path, DELETE | FILE_WRITE_ATTRIBUTES, share_all);
            try { remove(file, true); }
            catch (Error) {
                FILE_BASIC_INFO info{};
                if (GetFileInformationByHandleEx(file.get(), FileBasicInfo, &info, sizeof info) &&
                    (info.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
                    info = {};
                    info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                    check(SetFileInformationByHandle(file.get(), FileBasicInfo, &info, sizeof info), L"Clear temporary file attributes");
                }
                remove(file);
            }
            owned = false;
        } catch (Error) {}
    }
};

struct Report {
    Progress& progress;
    const std::atomic_bool& stop;
    uint64_t reported{};
#ifdef QCOPY_INSTRUMENTED
    DWORD simulated_error{};
#endif
    void append(uint64_t n) {
        reported += n;
        progress.transferred.fetch_add(n, std::memory_order_relaxed);
    }
};
COPYFILE2_MESSAGE_ACTION CALLBACK on_copy(const COPYFILE2_MESSAGE* event, PVOID context) {
    auto& report = *static_cast<Report*>(context);
    if (report.stop.load(std::memory_order_relaxed)) return COPYFILE2_PROGRESS_CANCEL;
    if (event->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
#ifdef QCOPY_INSTRUMENTED
        try { inject(L"write", {}); }
        catch (Error e) { report.simulated_error = e.code; return COPYFILE2_PROGRESS_CANCEL; }
#endif
        const auto done = event->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        if (done > report.reported) report.append(done - report.reported);
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}
bool unavailable(DWORD error) {
    return error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_PARAMETER;
}
void native(const std::wstring& source, Temporary& temp, DWORD flags, Report& report) {
    COPYFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof params;
    params.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK | COPY_FILE_DONT_REQUEST_DEST_WRITE_DAC;
    params.dwCopyFlags |= flags;
    params.pProgressRoutine = on_copy;
    params.pvCallbackContext = &report;
    const HRESULT hr = CopyFile2(source.c_str(), temp.path.c_str(), &params);
    if (SUCCEEDED(hr)) { temp.owned = true; return; }
    const DWORD e = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
    temp.owned = e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS;
    cancelled(report.stop);
#ifdef QCOPY_INSTRUMENTED
    if (report.simulated_error) fail(L"Write target", report.simulated_error);
#endif
    fail(L"CopyFile2", e);
}

struct Block {
    OVERLAPPED operation{};
    std::unique_ptr<std::byte[]> buffer;
    uint64_t offset{};
    DWORD size{};
    bool writing{};
};
static_assert(offsetof(Block, operation) == 0);
void iocp(const std::wstring& source, Temporary& temp, const Info& info, Report& report) {
    static std::mutex buffer_budget;
    const std::lock_guard budget(buffer_budget);
    cancelled(report.stop);
    auto input = open(source, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN);
    auto output = open(temp.path, GENERIC_WRITE, FILE_SHARE_READ, FILE_FLAG_OVERLAPPED, CREATE_NEW);
    temp.owned = true;
    FILE_END_OF_FILE_INFO size{};
    size.EndOfFile.QuadPart = static_cast<LONGLONG>(info.size());
    check(SetFileInformationByHandle(output.get(), FileEndOfFileInfo, &size, sizeof size), L"Size temporary file");
    Handle port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    check(port ? TRUE : FALSE, L"Create completion port");
    check(CreateIoCompletionPort(input.get(), port.get(), 0, 0) != nullptr, L"Associate source with completion port");
    check(CreateIoCompletionPort(output.get(), port.get(), 0, 0) != nullptr, L"Associate target with completion port");
    constexpr DWORD chunk = 4 * 1024 * 1024;
    std::array<Block, 8> blocks{};
    for (auto& block : blocks) block.buffer = std::make_unique_for_overwrite<std::byte[]>(chunk);
    uint64_t next = 0;
    unsigned pending = 0;
    const auto post = [&](Block& b, bool write) {
        b.operation = {};
        b.operation.Offset = static_cast<DWORD>(b.offset);
        b.operation.OffsetHigh = static_cast<DWORD>(b.offset >> 32);
        b.writing = write;
        const BOOL started = write ? WriteFile(output.get(), b.buffer.get(), b.size, nullptr, &b.operation)
                                   : ReadFile(input.get(), b.buffer.get(), b.size, nullptr, &b.operation);
        if (!started && GetLastError() != ERROR_IO_PENDING) fail(write ? L"Submit block write" : L"Submit block read");
        ++pending;
    };
    const auto read_next = [&](Block& b) {
        if (next == info.size()) return;
        b.offset = next;
        b.size = static_cast<DWORD>(std::min<uint64_t>(chunk, info.size() - next));
        next += b.size;
        post(b, false);
    };
    try {
        for (auto& b : blocks) read_next(b);
#ifdef QCOPY_INSTRUMENTED
        inject(L"blocks-pending", source);
#endif
        while (pending) {
            cancelled(report.stop);
            DWORD n{};
            ULONG_PTR key{};
            OVERLAPPED* operation = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(port.get(), &n, &key, &operation, 50);
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!operation) {
                if (error == WAIT_TIMEOUT) continue;
                fail(L"Receive block completion", error);
            }
            auto& b = *reinterpret_cast<Block*>(operation);
            --pending;
            if (!ok) fail(b.writing ? L"Write block" : L"Read block", error);
            if (n != b.size) fail(L"Short block transfer", ERROR_HANDLE_EOF);
            if (b.writing) { report.append(n); read_next(b); }
            else post(b, true);
        }
    } catch (...) {
        CancelIoEx(input.get(), nullptr);
        CancelIoEx(output.get(), nullptr);
        // Do not release OVERLAPPED storage until every request has completed.
        while (pending) {
            DWORD bytes{};
            ULONG_PTR key{};
            OVERLAPPED* completed = nullptr;
            GetQueuedCompletionStatus(port.get(), &bytes, &key, &completed, INFINITE);
            if (completed) --pending;
        }
        throw;
    }
    FILE_BASIC_INFO basic = info.basic;
    basic.ChangeTime.QuadPart = 0;
    check(SetFileInformationByHandle(output.get(), FileBasicInfo, &basic, sizeof basic), L"Set copied file attributes");
}

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm_{};
public:
    Sha256() { check_status(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)); }
    ~Sha256() { BCryptCloseAlgorithmProvider(algorithm_, 0); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    static void check_status(NTSTATUS status) { if (status < 0) fail(L"Compute SHA-256", ERROR_GEN_FAILURE); }
    std::array<UCHAR, 32> hash(const std::wstring& path, std::vector<UCHAR>& buffer, const std::atomic_bool& stop) const {
        struct Hash {
            BCRYPT_HASH_HANDLE handle{};
            ~Hash() { if (handle) BCryptDestroyHash(handle); }
        } hash;
        check_status(BCryptCreateHash(algorithm_, &hash.handle, nullptr, 0, nullptr, 0, 0));
        auto file = open(path, GENERIC_READ, share_all, FILE_FLAG_SEQUENTIAL_SCAN);
        for (;;) {
            cancelled(stop);
            DWORD n{};
            check(ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr), L"Read for SHA-256");
            if (!n) break;
            check_status(BCryptHashData(hash.handle, buffer.data(), n, 0));
        }
        std::array<UCHAR, 32> result{};
        check_status(BCryptFinishHash(hash.handle, result.data(), static_cast<ULONG>(result.size()), 0));
        return result;
    }
};
void verify(const std::wstring& source, const std::wstring& target,
            std::vector<std::wstring> names, const std::atomic_bool& stop) {
    names.emplace_back();
    std::vector<UCHAR> buffer(128 * 1024);
    const Sha256 sha;
    for (const auto& stream : names)
        if (sha.hash(source + stream, buffer, stop) != sha.hash(target + stream, buffer, stop))
            fail(L"SHA-256 mismatch", ERROR_CRC);
}

Handle ready_to_publish(const std::wstring& path, bool flush) {
    const DWORD flush_access = flush ? GENERIC_WRITE : 0;
    return open(path, DELETE | FILE_READ_ATTRIBUTES | flush_access, FILE_SHARE_READ);
}
}

DirectoryMetadata directory_metadata(const Handle& directory, const Volume& from, const Volume& to) {
    DirectoryMetadata metadata;
    metadata.names = streams(directory, from);
    if (!metadata.names.empty() && !(to.flags & FILE_NAMED_STREAMS)) fail(L"Target does not support directory streams", ERROR_NOT_SUPPORTED);
    if (from.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) metadata.eas = read_eas(directory);
    if (!metadata.eas.empty() && !(to.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES)) fail(L"Target does not support directory extended attributes", ERROR_NOT_SUPPORTED);
    return metadata;
}
void write_directory_metadata(const std::wstring& source, const std::wstring& target, DirectoryMetadata& metadata,
                              const Job& job, Progress& progress, const std::atomic_bool& stop) {
    const bool moving = job.operation == Operation::move;
    if (!metadata.eas.empty()) {
        const auto dir = open(target, FILE_WRITE_EA, share_all);
        write_eas(dir, metadata.eas);
    }
    if (metadata.names.empty()) return;
    std::vector<UCHAR> buffer(128 * 1024);
    for (const auto& name : metadata.names) {
        const auto input_path = source + name;
        const auto output_path = target + name;
        auto input = open(input_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_FLAG_SEQUENTIAL_SCAN);
        {
            const auto output = open(output_path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW);
            for (;;) {
                cancelled(stop);
                DWORD read{}, wrote{};
                check(ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr), L"Read directory data stream");
                if (!read) break;
                check(WriteFile(output.get(), buffer.data(), read, &wrote, nullptr), L"Write directory data stream");
                if (wrote != read) fail(L"Short write in directory data stream", ERROR_WRITE_FAULT);
                progress.transferred.fetch_add(wrote, std::memory_order_relaxed);
            }
            if (moving) check(FlushFileBuffers(output.get()), L"Flush directory data stream");
        }
        if (job.verify) {
            const Sha256 sha;
            if (sha.hash(input_path, buffer, stop) != sha.hash(output_path, buffer, stop)) fail(L"Directory stream SHA-256 mismatch", ERROR_CRC);
        }
        if (moving) metadata.locked_streams.push_back(std::move(input));
    }
}
void check_directory_metadata(const DirectoryMetadata& metadata, const Handle& source, const Volume& from) {
    auto names = streams(source, from);
    if (!std::is_permutation(names.begin(), names.end(), metadata.names.begin(), metadata.names.end()) ||
        ((from.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) && read_eas(source) != metadata.eas))
        fail(L"Source directory metadata changed", ERROR_FILE_INVALID);
}

void transfer(const std::wstring& source, const std::wstring& target, const std::wstring& directory,
              Handle source_lock, const Info& original, const Volume& from, const Volume& to,
              const Job& job, bool replace, Progress& progress, const std::atomic_bool& stop) {
    cancelled(stop);
    const bool moving = job.operation == Operation::move;
    constexpr DWORD ordinary = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    bool custom = job.strategy == Strategy::experimental_iocp && from.ntfs && to.ntfs &&
        from.internal_ssd && to.internal_ssd && from.disk != to.disk && original.size() >= large &&
        !(original.basic.FileAttributes & ~ordinary);
#ifdef QCOPY_INSTRUMENTED
    if (job.strategy == Strategy::experimental_iocp && from.ntfs && to.ntfs && original.size() >= large &&
        !(original.basic.FileAttributes & ~ordinary)) custom = true;
#endif
    const bool check_stream_capability = (from.flags & FILE_NAMED_STREAMS) && !(to.flags & FILE_NAMED_STREAMS);
    auto names = (custom || moving || job.verify || check_stream_capability) ? streams(source_lock, from) : std::vector<std::wstring>{};
    if (check_stream_capability && !names.empty()) fail(L"Target does not support named streams", ERROR_NOT_SUPPORTED);
    if ((from.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) && !(to.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) && has_eas(source_lock))
        fail(L"Target does not support extended attributes", ERROR_NOT_SUPPORTED);
    if (custom) {
        try { custom = names.empty() && !has_eas(source_lock); }
        catch (Error) { custom = false; }
    }
    Temporary first(directory);
    Report report{progress, stop};
    DWORD flags = (original.basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) && (to.flags & FILE_SUPPORTS_SPARSE_FILES)
        ? COPY_FILE_ENABLE_SPARSE_COPY : 0;
    const bool unbuffered = job.strategy == Strategy::native_unbuffered && original.size() >= large;
    bool retry = false;
    try {
        if (custom) iocp(source, first, original, report);
        else native(source, first, flags | (unbuffered ? COPY_FILE_NO_BUFFERING : 0), report);
    } catch (Error e) {
        if ((!custom && !unbuffered) || !unavailable(e.code)) throw;
        retry = true;
    }
    std::unique_ptr<Temporary> fallback;
    if (retry) {
        first.cleanup();
        fallback = std::make_unique<Temporary>(directory);
        report.reported = 0;
        native(source, *fallback, flags, report);
    }
    auto& temp = retry ? *fallback : first;
    if (original.size() > report.reported) report.append(original.size() - report.reported);
    // Copy uses one protected destination handle for length validation and publish.
    auto pending = moving ? open(temp.path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, share_all)
                          : ready_to_publish(temp.path, false);
    BY_HANDLE_FILE_INFORMATION copied{};
    check(GetFileInformationByHandle(pending.get(), &copied), L"Inspect copied file");
    if ((copied.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        ((uint64_t{copied.nFileSizeHigh} << 32) | copied.nFileSizeLow) != original.size())
        fail(L"Check copied file size and type", ERROR_INVALID_DATA);
    if (job.verify) {
        const auto target_names = streams(pending, to);
        if (!std::is_permutation(names.begin(), names.end(), target_names.begin(), target_names.end()))
            fail(L"Named streams differ", ERROR_CRC);
        verify(source, temp.path, names, stop);
    }
    if (moving) {
        const Info expected(pending);
        FILE_BASIC_INFO attributes{};
        const bool readonly = (copied.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (readonly) {
            attributes.FileAttributes = copied.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (!attributes.FileAttributes) attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            check(SetFileInformationByHandle(pending.get(), FileBasicInfo, &attributes, sizeof attributes), L"Make staged file writable for flush");
        }
        auto writable = ready_to_publish(temp.path, true);
        if (!expected.same(Info(writable))) fail(L"Temporary file was replaced", ERROR_FILE_INVALID);
        pending = std::move(writable);
        for (const auto& name : names) {
            cancelled(stop);
            const auto stream = open(temp.path + name, GENERIC_WRITE, share_all);
            check(FlushFileBuffers(stream.get()), L"Flush named data stream before removing source");
        }
        if (readonly) {
            attributes.FileAttributes = copied.dwFileAttributes;
            check(SetFileInformationByHandle(pending.get(), FileBasicInfo, &attributes, sizeof attributes), L"Restore read-only attribute");
        }
        check(FlushFileBuffers(pending.get()), L"Flush target before removing source");
    }
    if ((moving || job.verify) && !original.unchanged(Info(source_lock))) fail(L"Source changed during transfer", ERROR_FILE_INVALID);
    cancelled(stop);
#ifdef QCOPY_INSTRUMENTED
    inject(L"before-publish", source);
#endif
    rename(pending, target, replace);
    temp.owned = false;
    if (!retry && custom) progress.iocp_completed.fetch_add(1, std::memory_order_relaxed);
    if (!retry && !custom && unbuffered) progress.unbuffered_completed.fetch_add(1, std::memory_order_relaxed);
    if (moving) {
        const auto identity_pin = open(source, FILE_READ_ATTRIBUTES, share_all);
        if (!original.same(Info(identity_pin))) fail(L"Target published; source was replaced", ERROR_FILE_INVALID);
        source_lock.reset();
#ifdef QCOPY_INSTRUMENTED
        inject(L"after-publish", source);
#endif
        try {
            auto source_delete = open(source, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
            const Info current(source_delete);
            if (current.link() || !original.unchanged(current)) fail(L"Target published; source changed", ERROR_FILE_INVALID);
            remove(source_delete, true);
        } catch (Error e) { fail(L"Target published; source retained", e.code); }
    }
}
}
