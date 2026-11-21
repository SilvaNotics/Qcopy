#pragma once
#include "win.h"

namespace qc::win {
struct DirectoryMetadata {
    std::vector<std::wstring> names;
    std::vector<std::byte> eas;
    std::vector<Handle> locked_streams;
};
DirectoryMetadata directory_metadata(const Handle&, const Volume& from, const Volume& to);
void write_directory_metadata(const std::wstring& source, const std::wstring& target, DirectoryMetadata&,
                              const Job&, Progress&, const std::atomic_bool& stop);
void check_directory_metadata(const DirectoryMetadata&, const Handle&, const Volume&);
void transfer(const std::wstring& source, const std::wstring& target, const std::wstring& target_directory,
              Handle source_lock, const Info& original, const Volume& from, const Volume& to,
              const Job&, bool replace, Progress&, const std::atomic_bool& stop);
}
