#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qc {

enum class Operation { copy, move };
enum class Conflict { skip, replace, rename };
enum class Strategy { automatic, native_single, native_unbuffered, experimental_iocp };

struct Job {
    std::vector<std::wstring> sources;
    std::wstring destination;
    Operation operation = Operation::copy;
    Conflict conflict = Conflict::skip;
    bool verify = false;
    Strategy strategy = Strategy::automatic;
};
struct Progress {
    std::atomic_uint64_t discovered{}, completed{}, transferred{}, failed{}, skipped{};
    std::atomic_bool enumeration_done{};
    std::atomic_uint64_t iocp_completed{}, unbuffered_completed{};
};
struct Issue {
    std::wstring path;
    std::wstring stage;
    unsigned long code;
    bool skipped;
};
struct Result {
    bool cancelled;
    uint64_t failed, skipped;
    int exit_code() const { return cancelled ? 130 : (failed || skipped ? 1 : 0); }
};

// Issue callbacks are serialized across workers and must not throw.
Result run(const Job&, Progress&, const std::atomic_bool& cancel,
           const std::function<void(const Issue&)>& on_issue);
std::wstring error_message(unsigned long code, unsigned long language = 0);
std::wstring display_path(const std::wstring&);
}
