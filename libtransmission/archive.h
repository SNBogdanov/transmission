// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct tr_error;
struct tr_session;
struct tr_torrent;

namespace tr::archive
{

inline constexpr std::string_view AllArchives = "all";

struct Entry
{
    std::string archive;
    std::string hash_string;
    std::string name;
    std::string download_dir;
    std::vector<std::string> labels;
    int64_t added_date = 0;
    int64_t archived_date = 0;
    uint64_t total_size = 0;
    bool local_data_deleted = false;
    bool has_torrent = false;
    bool has_resume = false;
    bool has_metadata = false;
    bool has_magnet = false;
};

enum class SortBy
{
    None,
    Name,
    TotalSize,
    AddedDate,
    ArchivedDate,
    DownloadDir,
};

enum class SortOrder
{
    Ascending,
    Descending,
};

struct ListRequest
{
    size_t offset = 0;
    size_t limit = 1000;
    SortBy sort_by = SortBy::None;
    SortOrder sort_order = SortOrder::Ascending;
    std::string label;
};

struct ListResult
{
    size_t total = 0;
    std::vector<Entry> entries;
};

struct ExportResult
{
    std::string path;
    std::string sha256;
    uint64_t size = 0;
    size_t count = 0;
};

struct DeleteResult
{
    std::string archive;
    std::string hash_string;
    size_t files_removed = 0;
};

struct ViewResult
{
    std::string archive;
    std::string hash_string;
    std::string name;
    std::string filename;
    std::string data;
    std::vector<std::string> files;
    uint64_t size = 0;
};

struct ImportResult
{
    std::vector<std::string> hashes;
    size_t skipped = 0;
};

struct RestoreResult
{
    std::string archive;
    std::string hash_string;
    bool removed = false;
    std::string remove_error;
};

struct RestoreRequest
{
    std::string archive;
    std::string hash_string;
    std::string download_dir;
    bool remove_from_archive = false;
    bool verify = false;
};

struct RestoreError
{
    std::string archive;
    std::string hash_string;
    std::string message;
};

struct RestoreBatchResult
{
    size_t restored = 0;
    std::vector<RestoreResult> entries;
    std::vector<RestoreError> errors;
};

struct MoveResult
{
    size_t moved = 0;
    std::vector<std::string> missing;
};

[[nodiscard]] bool is_valid_name(std::string_view name);

[[nodiscard]] std::string archive_path(tr_session const& session, std::string_view archive_name);
[[nodiscard]] std::string export_path(tr_session const& session, std::string_view archive_name, std::string_view filename);

// All functions below must be called from the session thread.
[[nodiscard]] bool list_archives(tr_session& session, std::vector<std::string>& names, tr_error* error = nullptr);

[[nodiscard]] bool create_archive(tr_session& session, std::string_view archive_name, bool& created, tr_error* error = nullptr);

[[nodiscard]] bool delete_archive(tr_session& session, std::string_view archive_name, tr_error* error = nullptr);

[[nodiscard]] bool delete_entry(
    tr_session& session,
    std::string_view archive_name,
    std::string_view hash_string,
    DeleteResult& result,
    tr_error* error = nullptr);

[[nodiscard]] bool view(
    tr_session& session,
    std::string_view archive_name,
    std::string_view hash_string,
    ViewResult& result,
    tr_error* error = nullptr);

[[nodiscard]] bool list(
    tr_session& session,
    std::string_view archive_name,
    ListRequest const& request,
    ListResult& result,
    tr_error* error = nullptr);

// Must be called from the session thread.
[[nodiscard]] bool save(
    tr_session& session,
    tr_torrent& tor,
    std::string_view archive_name,
    bool delete_local_data,
    bool overwrite,
    tr_error* error = nullptr);

// Must be called from the session thread.
[[nodiscard]] bool restore(
    tr_session& session,
    std::string_view archive_name,
    std::string_view hash_string,
    std::string_view download_dir,
    bool remove_from_archive,
    bool verify,
    RestoreResult& result,
    tr_error* error = nullptr);

// Restores each request in order. Per-entry failures are reported in
// RestoreBatchResult::errors and do not stop the remaining restores.
[[nodiscard]] bool restore(
    tr_session& session,
    std::vector<RestoreRequest> const& requests,
    RestoreBatchResult& result,
    tr_error* error = nullptr);

[[nodiscard]] bool move(
    tr_session& session,
    std::string_view source_archive,
    std::string_view target_archive,
    std::vector<std::string> const& hashes,
    bool overwrite,
    MoveResult& result,
    tr_error* error = nullptr);

[[nodiscard]] bool export_bundle(
    tr_session& session,
    std::string_view archive_name,
    std::vector<std::string> const& hashes,
    std::string_view output_path,
    bool overwrite,
    ExportResult& result,
    tr_error* error = nullptr);

[[nodiscard]] bool import_bundle(
    tr_session& session,
    std::string_view bundle_path,
    std::string_view archive_name,
    bool overwrite,
    ImportResult& result,
    tr_error* error = nullptr);

} // namespace tr::archive
