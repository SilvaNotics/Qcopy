#include "win.h"
#include <filesystem>
#include <array>
#include <cstring>
#include <iostream>

int wmain(int count, wchar_t** args) {
    if (count != 2) return 2;
    using namespace qc;
    using namespace qc::win;
    const auto parent = absolute(args[1]);
    const auto root = temporary(parent);
    struct Fixture {
        std::wstring path;
        ~Fixture() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } fixture{root};
    try {
        check(CreateDirectoryW(root.c_str(), nullptr), L"Create fixture");
        const auto input = join(root, L"源");
        const auto output = join(root, L"目标");
        check(CreateDirectoryW(input.c_str(), nullptr), L"Create source");
        check(CreateDirectoryW(output.c_str(), nullptr), L"Create destination");
        const auto local = volume(input);
        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        const bool carries_streams = (local.flags & FILE_NAMED_STREAMS) != 0;
        const bool carries_eas = (local.flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) != 0;
        std::vector<std::byte> expected_eas;
        std::vector<std::byte> packed_eas;
        if (carries_streams) {
            auto stream = open(input + L":qcopy-test:$DATA", GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW);
            DWORD n{};
            check(WriteFile(stream.get(), "folder metadata", 15, &n, nullptr), L"Write directory stream");
        }
        if (carries_eas) {
            const std::array<unsigned char, 15> packed{0, 0, 0, 0, 0, 4, 2, 0, 'T', 'E', 'S', 'T', 0, 'O', 'K'};
            packed_eas.resize(packed.size());
            std::memcpy(packed_eas.data(), packed.data(), packed_eas.size());
            const auto dir = open(input, FILE_READ_EA | FILE_WRITE_EA, share);
            write_eas(dir, packed_eas);
            expected_eas = read_eas(dir);
            if (expected_eas.empty()) return 1;
        }
        auto current = input;
        for (int i = 0; i != 8; ++i) {
            current = join(current, L"长文件名路径_abcdefghijklmnopqrstuvwxyz_0123456789");
            check(CreateDirectoryW(current.c_str(), nullptr), L"Create extended-path subdirectory");
        }
        const auto name = L"你好_🗂.txt";
        const auto original = join(current, name);
        {
            auto file = open(original, GENERIC_WRITE, 0, 0, CREATE_NEW);
            if (carries_eas) write_eas(file, packed_eas);
            DWORD n{};
            check(WriteFile(file.get(), "qcopy", 5, &n, nullptr), L"Write Unicode test data");
            if (n != 5) fail(L"Write Unicode test data", ERROR_WRITE_FAULT);
        }
        Progress progress;
        const std::atomic_bool stop{};
        const auto result = run(Job{{input}, output, Operation::copy, Conflict::skip, true}, progress, stop,
            [](const Issue& i) { std::wcerr << i.stage << L" (" << i.code << L")\n"; });
        if (result.exit_code()) return 1;
        const auto copy = output + original.substr(root.size());
        const auto file = open(copy, GENERIC_READ, FILE_SHARE_READ);
        char payload[8]{};
        DWORD n{};
        check(ReadFile(file.get(), payload, sizeof payload, &n, nullptr), L"Read Unicode test data");
        if (n != 5 || std::string_view(payload, n) != "qcopy") return 1;
        if (carries_eas && read_eas(file) != expected_eas) return 1;
        const auto target_directory = join(output, L"源");
        if (carries_streams) {
            const auto stream = open(target_directory + L":qcopy-test:$DATA", GENERIC_READ, FILE_SHARE_READ);
            char value[32]{};
            check(ReadFile(stream.get(), value, sizeof value, &n, nullptr), L"Read directory stream");
            if (std::string_view(value, n) != "folder metadata") return 1;
        }
        if (carries_eas) {
            const auto dir = open(target_directory, FILE_READ_EA, share);
            if (read_eas(dir) != expected_eas) return 1;
        }
        std::cout << "Unicode paths, directory named streams, and directory extended attributes passed\n";
    } catch (Error e) {
        std::wcerr << e.stage << L" (" << e.code << L")\n";
        return 1;
    }
}
