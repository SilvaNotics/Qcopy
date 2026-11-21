#include "transfer.h"
#include <array>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace qc {
namespace {
using namespace win;
constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD directory_access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES;

class Queue {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> items_;
    unsigned active_{};
    unsigned limit_ = 1;
    bool closing_{};
    std::vector<std::jthread> workers_;
public:
    Queue() = default;
    ~Queue() {
        { std::lock_guard hold(mutex_); closing_ = true; }
        changed_.notify_all();
    }
    void start(unsigned count) {
        { std::lock_guard hold(mutex_); limit_ = count; }
        while (workers_.size() < count) workers_.emplace_back([this] {
            for (;;) {
                {
                    std::function<void()> task;
                    {
                        std::unique_lock hold(mutex_);
                        changed_.wait(hold, [&] { return closing_ || (!items_.empty() && active_ < limit_); });
                        if (items_.empty()) return;
                        task = std::move(items_.front());
                        items_.pop_front();
                        ++active_;
                    }
                    changed_.notify_all();
                    task();
                }
                { std::lock_guard hold(mutex_); --active_; }
                changed_.notify_all();
            }
        });
    }
    void submit(std::function<void()> task) {
        std::unique_lock hold(mutex_);
        changed_.wait(hold, [&] { return items_.size() < 128; });
        items_.push_back(std::move(task));
        hold.unlock();
        changed_.notify_all();
    }
    void drain() {
        std::unique_lock hold(mutex_);
        changed_.wait(hold, [&] { return items_.empty() && !active_; });
    }
};

struct Destination {
    std::wstring path;
    bool replace{};
};

class Engine {
    const Job& job_;
    Progress& progress_;
    const std::atomic_bool& stop_;
    const std::function<void(const Issue&)>& on_issue_;
    std::mutex issues_;
    Volume to_;
    Queue queue_;

    struct Directory {
        Engine& engine;
        std::shared_ptr<Directory> parent;
        std::wstring source, target;
        Handle source_lock, target_lock;
        Info identity;
        Volume from;
        DirectoryMetadata metadata;
        bool traversed{};
        std::atomic_bool incomplete{};

        Directory(Engine& e, std::shared_ptr<Directory> p, const std::wstring& from, const std::wstring& to,
                  Handle source_h, Handle target_h, const Info& source_id, Volume local, DirectoryMetadata meta)
            : engine(e), parent(std::move(p)), source(from), target(to), source_lock(std::move(source_h)),
              target_lock(std::move(target_h)), identity(source_id), from(local), metadata(std::move(meta)) {}
        ~Directory() { engine.finish_directory(*this); }
    };

    bool moving() const { return job_.operation == Operation::move; }
    void issue(const std::wstring& path, Error e, bool skipped = false) {
        if (e.code == ERROR_CANCELLED && stop_) return;
        (skipped ? progress_.skipped : progress_.failed).fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(issues_);
        if (on_issue_) on_issue_({display_path(path), e.stage, e.code, skipped});
    }
    void mark_complete() { progress_.completed.fetch_add(1, std::memory_order_relaxed); }

    Destination choose(const std::wstring& directory, const std::wstring& name, const Handle& source_handle, const Info& source) {
        auto target = join(directory, name);
        std::wstring stem = name, ext;
        const auto dot = name.rfind(L'.');
        if (!source.dir() && dot != std::wstring::npos && dot > 0) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
        for (unsigned suffix = 1;; ++suffix) {
            try {
                const auto existing = open(target, FILE_READ_ATTRIBUTES, share_all);
                const Info metadata(existing);
                if (Info(source_handle).same(metadata)) fail(L"Source and target are the same item", ERROR_INVALID_DATA);
                if (job_.conflict == Conflict::skip || (job_.conflict == Conflict::replace &&
                    (source.dir() || metadata.dir() || metadata.link())))
                    fail(L"Name conflict", ERROR_ALREADY_EXISTS);
                if (job_.conflict == Conflict::replace) return {std::move(target), true};
            } catch (Error e) {
                if (missing(e.code)) return {std::move(target), false};
                throw;
            }
            cancelled(stop_);
            if (suffix == 100000) fail(L"No available name", ERROR_ALREADY_EXISTS);
            target = join(directory, stem + L" (" + std::to_wstring(suffix) + L")" + ext);
        }
    }

    bool rename_on_volume(const std::wstring& source, const Destination& target, const Info& original) {
        auto file = open(source, FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | (original.dir() ? FILE_SHARE_WRITE : 0));
        const Info current(file);
        if (!original.same(current) || current.link() || original.dir() != current.dir()) fail(L"Source was replaced", ERROR_FILE_INVALID);
        cancelled(stop_);
        try { rename(file, target.path, target.replace); }
        catch (Error e) { if (e.code == ERROR_NOT_SAME_DEVICE) return false; throw; }
        return true;
    }
    void file(const std::wstring& source, const std::wstring& directory, const Volume& from,
              bool try_rename, bool allow_replace = true, const Info* expected = nullptr) {
        cancelled(stop_);
        auto lock = open(source, FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA, FILE_SHARE_READ);
        const bool detailed = moving() || job_.verify || expected || job_.strategy == Strategy::experimental_iocp;
        const Info original(lock, detailed);
        if (expected && !original.same(*expected)) fail(L"Source was replaced", ERROR_FILE_INVALID);
        if (original.link()) fail(L"Reparse point skipped", ERROR_REPARSE_TAG_INVALID);
        if (original.dir()) fail(L"Source changed type", ERROR_FILE_INVALID);
        auto target = choose(directory, leaf(source), lock, original);
        if (target.replace && !allow_replace) fail(L"Replacing files from a case-sensitive directory is ambiguous", ERROR_ALREADY_EXISTS);
        if (moving() && try_rename) {
            auto pin = open(source, FILE_READ_ATTRIBUTES, share_all);
            lock.reset();
            if (rename_on_volume(source, target, original)) return;
            lock = open(source, FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA, FILE_SHARE_READ);
            if (!original.unchanged(Info(lock))) fail(L"Source changed before transfer", ERROR_FILE_INVALID);
        }
        transfer(source, target.path, directory, std::move(lock), original, from, to_, job_, target.replace, progress_, stop_);
    }
    void finish_directory(Directory& dir) {
        if (!dir.traversed || stop_.load(std::memory_order_relaxed)) {
            if (dir.parent) dir.parent->incomplete = true;
            mark_complete();
            return;
        }
        try {
            auto attributes = dir.identity.basic;
            attributes.ChangeTime.QuadPart = 0;
            attributes.FileAttributes &= FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            check(SetFileInformationByHandle(dir.target_lock.get(), FileBasicInfo, &attributes, sizeof attributes), L"Set directory attributes");
            if (moving() && !dir.incomplete) {
                const auto pin = open(dir.source, FILE_READ_ATTRIBUTES, share_all);
                dir.source_lock.reset();
                auto erase = open(dir.source, FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE);
                if (!dir.identity.same(Info(erase))) fail(L"Source directory was replaced", ERROR_FILE_INVALID);
                check_directory_metadata(dir.metadata, erase, dir.from);
                remove(erase, true);
            }
        } catch (Error e) {
            dir.incomplete = true;
            issue(dir.source, e);
        }
        if (dir.parent && dir.incomplete) dir.parent->incomplete = true;
        mark_complete();
    }
    void visit(const std::wstring& source, const std::wstring& target_parent, const Volume& from,
               std::shared_ptr<Directory> parent, bool try_rename, unsigned concurrency, const Info* expected = nullptr) {
        progress_.discovered.fetch_add(1, std::memory_order_relaxed);
        bool managed = false;
        try {
            cancelled(stop_);
            auto input = open(source, FILE_READ_ATTRIBUTES | FILE_READ_EA, share_all);
            const Info metadata(input);
            if (expected && !metadata.same(*expected)) fail(L"Source was replaced", ERROR_FILE_INVALID);
            if (metadata.link()) fail(L"Reparse point skipped", ERROR_REPARSE_TAG_INVALID);
            if (!metadata.dir()) {
                file(source, target_parent, from, try_rename, true, &metadata);
            } else {
                const auto target = choose(target_parent, leaf(source), input, metadata);
                if (!moving() || !try_rename || !rename_on_volume(source, target, metadata)) {
                    auto scan = open(source, directory_access | FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE);
                    if (!metadata.same(Info(scan))) fail(L"Source directory was replaced", ERROR_FILE_INVALID);
                    auto directory_data = directory_metadata(scan, from, to_);
                    check(CreateDirectoryW(target.path.c_str(), nullptr), L"Create target directory");
                    auto target_lock = open(target.path, directory_access | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE);
                    if (Info(target_lock).link()) fail(L"Target directory is a reparse point", ERROR_FILE_INVALID);
                    write_directory_metadata(source, target.path, directory_data, job_, progress_, stop_);
                    auto dir = std::make_shared<Directory>(*this, parent, source, target.path, std::move(scan),
                        std::move(target_lock), metadata, from, std::move(directory_data));
                    managed = true;
                    try {
                        queue_.start(concurrency);
                        enumerate(dir, from, concurrency);
                        dir->traversed = true;
                    } catch (Error e) {
                        dir->incomplete = true;
                        issue(source, e);
                    }
                }
            }
        } catch (Error e) {
            if (parent) parent->incomplete = true;
            issue(source, e, e.code == ERROR_ALREADY_EXISTS || e.code == ERROR_FILE_EXISTS || e.code == ERROR_REPARSE_TAG_INVALID);
        }
        if (!managed) mark_complete();
    }
    void enumerate(const std::shared_ptr<Directory>& dir, const Volume& from, unsigned concurrency) {
        FILE_CASE_SENSITIVE_INFO case_info{};
        const bool allow_replace = !GetFileInformationByHandleEx(dir->source_lock.get(), FileCaseSensitiveInfo, &case_info, sizeof case_info) ||
            !(case_info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
        const auto schedule = [&](std::wstring_view name, DWORD attributes) {
            cancelled(stop_);
            if (name == L"." || name == L"..") return;
            auto source = join(dir->source, name);
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                visit(source, dir->target, from, dir, false, concurrency);
                return;
            }
            progress_.discovered.fetch_add(1, std::memory_order_relaxed);
            queue_.submit([this, dir, source = std::move(source), from, allow_replace] {
                try { file(source, dir->target, from, false, allow_replace); }
                catch (Error e) {
                    dir->incomplete = true;
                    issue(source, e, e.code == ERROR_ALREADY_EXISTS || e.code == ERROR_FILE_EXISTS || e.code == ERROR_REPARSE_TAG_INVALID);
                } catch (const std::bad_alloc&) {
                    dir->incomplete = true;
                    issue(source, {ERROR_OUTOFMEMORY, L"Allocate copy buffers"});
                }
                mark_complete();
            });
        };
        std::vector<std::byte> buffer(32 * 1024);
        bool first = true;
        for (;;) {
            cancelled(stop_);
            if (!GetFileInformationByHandleEx(dir->source_lock.get(), FileFullDirectoryInfo,
                    buffer.data(), static_cast<DWORD>(buffer.size()))) {
                const DWORD error = GetLastError();
                if (error == ERROR_NO_MORE_FILES) return;
                if (first && (error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED)) break;
                fail(L"Enumerate directory", error);
            }
            first = false;
            size_t offset = 0;
            for (;;) {
                const auto* item = reinterpret_cast<const FILE_FULL_DIR_INFO*>(buffer.data() + offset);
                schedule(std::wstring_view(item->FileName, item->FileNameLength / sizeof(wchar_t)), item->FileAttributes);
                if (!item->NextEntryOffset) break;
                offset += item->NextEntryOffset;
            }
        }
        struct Find {
            HANDLE value;
            ~Find() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
        };
        WIN32_FIND_DATAW data{};
        const Find find{FindFirstFileExW(join(dir->source, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0)};
        if (find.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return;
            fail(L"Enumerate directory with FindFirstFileEx", error);
        }
        do { schedule(data.cFileName, data.dwFileAttributes); } while (FindNextFileW(find.value, &data));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) fail(L"Enumerate directory with FindNextFile", error);
    }
public:
    Engine(const Job& job, Progress& progress, const std::atomic_bool& stop, const std::function<void(const Issue&)>& cb)
        : job_(job), progress_(progress), stop_(stop), on_issue_(cb) {}
    Result run() {
        try {
            const auto raw = absolute(job_.destination);
            const auto target = open(raw, directory_access, FILE_SHARE_READ | FILE_SHARE_WRITE);
            const Info target_info(target);
            if (!target_info.dir() || target_info.link()) fail(L"Choose an existing local directory", ERROR_DIRECTORY);
            const auto canonical = final_path(target);
            to_ = volume(raw);
            for (size_t index = 0; index < job_.sources.size() && !stop_; ++index) {
                const auto& raw_source = job_.sources[index];
                try {
                    const auto requested = absolute(raw_source);
                    const auto source = open(requested, FILE_READ_ATTRIBUTES, share_all);
                    const Info source_info(source);
                    if (source_info.link()) fail(L"Reparse point skipped", ERROR_REPARSE_TAG_INVALID);
                    const auto resolved = final_path(source);
                    if (source_info.dir() && (source_info.same(target_info) || contains(resolved, canonical)))
                        fail(L"Destination is inside the source directory", ERROR_INVALID_DATA);
                    if (resolved.ends_with(L'\\')) fail(L"A volume root cannot be copied as an item", ERROR_INVALID_NAME);
                    const auto from = volume(requested);
                    const bool parallel = from.internal_ssd && to_.internal_ssd &&
                        (job_.strategy == Strategy::automatic || job_.strategy == Strategy::native_unbuffered);
                    visit(resolved, canonical, from, {}, moving(), parallel ? 4 : 1, &source_info);
                    if (index + 1 < job_.sources.size()) queue_.drain();
                } catch (Error e) {
                    issue(raw_source, e, e.code == ERROR_REPARSE_TAG_INVALID);
                }
            }
            progress_.enumeration_done = true;
            queue_.drain();
        } catch (Error e) {
            issue(job_.destination, e);
        } catch (const std::bad_alloc&) {
            issue(job_.destination, {ERROR_OUTOFMEMORY, L"Allocate copy job"});
        } catch (const std::system_error&) {
            issue(job_.destination, {ERROR_NO_SYSTEM_RESOURCES, L"Start copy workers"});
        }
        progress_.enumeration_done = true;
        queue_.drain();
        return {stop_.load(), progress_.failed.load(), progress_.skipped.load()};
    }
};
}
Result run(const Job& job, Progress& progress, const std::atomic_bool& cancel,
           const std::function<void(const Issue&)>& on_issue) {
    return Engine(job, progress, cancel, on_issue).run();
}
}
