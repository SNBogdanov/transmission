// This file Copyright Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†РІР‚С›РЎС›Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†Р вЂљРЎвЂќР В Р’В Р В Р вЂ№Р В Р Р‹Р Р†Р вЂљРЎвЂќР В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂєР В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В© Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "libtransmission/archive-bundle.h"
#include "libtransmission/archive.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/file-utils.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/torrent.h"
#include "libtransmission/transmission.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{

constexpr size_t MaxSidecarBytes = 1024U * 1024U;
constexpr size_t MaxManifestBytes = 64U * 1024U * 1024U;
constexpr size_t ChunkSize = 1024U * 1024U;

[[nodiscard]] tr_quark key(std::string_view const sv)
{
    return tr_quark_new(sv);
}

void set_error(tr_error* const error, int const code, std::string_view const message)
{
    if (error != nullptr)
    {
        error->set(static_cast<tr_error_code_t>(code), message);
    }
}

[[nodiscard]] bool has_error(tr_error const* const error)
{
    return error != nullptr && *error;
}

[[nodiscard]] bool is_hex(char const ch) noexcept
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

[[nodiscard]] std::string to_lower_hash(std::string_view const input)
{
    auto ret = std::string{ input };
    std::ranges::transform(ret, ret.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ret;
}

[[nodiscard]] bool is_hash_string(std::string_view const input) noexcept
{
    return std::size(input) == 40U && std::ranges::all_of(input, is_hex);
}

[[nodiscard]] std::string entry_filename(std::string_view const dir, std::string_view const hash, std::string_view const suffix)
{
    return fmt::format("{:s}/{:s}{:s}"sv, dir, hash, suffix);
}

[[nodiscard]] constexpr auto entry_suffixes() noexcept
{
    return std::array<std::string_view, 4>{ ".torrent"sv, ".resume"sv, ".json"sv, ".magnet"sv };
}

[[nodiscard]] bool has_any_entry(std::string_view const dir, std::string_view const hash)
{
    for (auto const suffix : entry_suffixes())
    {
        if (tr_sys_path_exists(entry_filename(dir, hash, suffix)))
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] std::vector<std::string> collect_hashes(std::string_view const dir)
{
    auto hashes = std::set<std::string>{};

    for (auto const& name : tr_sys_dir_get_files(dir))
    {
        for (auto const suffix : entry_suffixes())
        {
            if (std::size(name) == 40U + std::size(suffix) && name.ends_with(suffix))
            {
                auto const hash = name.substr(0U, 40U);
                if (is_hash_string(hash))
                {
                    hashes.emplace(to_lower_hash(hash));
                }
            }
        }
    }

    return { std::begin(hashes), std::end(hashes) };
}

[[nodiscard]] bool read_metadata(std::string_view const dir, std::string_view const hash, tr::archive::Entry& entry)
{
    auto const path = entry_filename(dir, hash, ".json"sv);
    auto const info = tr_sys_path_get_info(path);
    if (!info || !info->isFile() || info->size > MaxSidecarBytes)
    {
        return false;
    }

    auto const handle = tr_sys_file_open(path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, nullptr);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto contents = std::vector<char>(static_cast<size_t>(info->size));
    auto const ok = std::empty(contents) || tr_sys_file_read(handle, std::data(contents), std::size(contents), nullptr);
    tr_sys_file_close(handle);
    if (!ok)
    {
        return false;
    }

    auto parsed = tr_variant_serde::json().parse(std::string_view{ std::data(contents), std::size(contents) });
    if (!parsed)
    {
        return false;
    }

    auto const* const map = parsed->get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    entry.hash_string = hash;
    entry.name = map->value_if<std::string_view>(key("name")).value_or(hash);
    entry.download_dir = map->value_if<std::string_view>(key("download_dir")).value_or(""sv);
    entry.added_date = map->value_if<int64_t>(key("added_date")).value_or(0);
    entry.archived_date = map->value_if<int64_t>(key("archived_date")).value_or(0);
    entry.total_size = map->value_if<int64_t>(key("total_size")).value_or(0);
    entry.local_data_deleted = map->value_if<bool>(key("local_data_deleted")).value_or(false);

    if (auto const* const labels = map->find_if<tr_variant::Vector>(key("labels")); labels != nullptr)
    {
        for (auto const& label_var : *labels)
        {
            if (auto const label = label_var.value_if<std::string_view>(); label)
            {
                entry.labels.emplace_back(*label);
            }
        }
    }

    return true;
}

[[nodiscard]] std::string make_metadata_json(tr::archive::Entry const& entry)
{
    auto map = tr_variant::Map{};
    map.try_emplace(key("hash_string"), entry.hash_string);
    map.try_emplace(key("name"), entry.name);
    map.try_emplace(key("download_dir"), entry.download_dir);
    map.try_emplace(key("added_date"), entry.added_date);
    map.try_emplace(key("archived_date"), entry.archived_date);
    map.try_emplace(key("total_size"), static_cast<int64_t>(entry.total_size));
    map.try_emplace(key("local_data_deleted"), entry.local_data_deleted);
    map.try_emplace(key("has_torrent"), entry.has_torrent);
    map.try_emplace(key("has_resume"), entry.has_resume);
    map.try_emplace(key("has_magnet"), entry.has_magnet);

    auto labels = tr_variant::make_vector(entry.labels.size());
    if (auto* const vec = labels.get_if<tr_variant::Vector>(); vec != nullptr)
    {
        for (auto const& label : entry.labels)
        {
            vec->emplace_back(label);
        }
    }
    map.try_emplace(key("labels"), std::move(labels));

    auto const root = tr_variant{ std::move(map) };
    return tr_variant_serde::json().compact().to_string(root);
}

[[nodiscard]] bool copy_file_atomic(std::string_view const source, std::string_view const destination, tr_error* error)
{
    auto const tmp = fmt::format("{:s}.{:d}.tmp"sv, destination, static_cast<int64_t>(std::time(nullptr)));
    tr_sys_path_remove(tmp, nullptr);

    if (!tr_sys_path_copy(source, tmp, error))
    {
        return false;
    }

    if (tr_sys_path_rename(tmp, destination, error))
    {
        return true;
    }

    tr_sys_path_remove(tmp, nullptr);
    return false;
}

struct ScratchDir
{
    std::string path;
    std::vector<std::string> files;

    ScratchDir() = default;
    ScratchDir(ScratchDir const&) = delete;
    ScratchDir& operator=(ScratchDir const&) = delete;

    ~ScratchDir()
    {
        for (auto const& file : files)
        {
            tr_sys_path_remove(file, nullptr);
        }

        if (!std::empty(path))
        {
            tr_sys_path_remove(path, nullptr);
        }
    }
};

[[nodiscard]] std::string make_temp_dir(std::string_view const parent, std::string_view const prefix, tr_error* const error)
{
    auto path = tr_pathbuf{ fmt::format("{:s}/{:s}XXXXXX"sv, parent, prefix) };
    return tr_sys_dir_create_temp(std::data(path), error) ? std::string{ path.sv() } : std::string{};
}

[[nodiscard]] bool sha256_file(std::string_view const path, std::string& out, tr_error* error)
{
    auto const info = tr_sys_path_get_info(path, 0, error);
    if (!info || !info->isFile())
    {
        if (!has_error(error))
        {
            set_error(error, ENOENT, "couldn't stat file"sv);
        }
        return false;
    }

    auto const handle = tr_sys_file_open(path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto sha = tr_sha256{};
    auto buffer = std::vector<char>(ChunkSize);
    auto offset = uint64_t{};
    auto ok = true;

    while (offset < info->size)
    {
        auto const n = static_cast<size_t>(std::min<uint64_t>(info->size - offset, std::size(buffer)));
        auto n_read = uint64_t{};
        if (!tr_sys_file_read_at(handle, std::data(buffer), n, offset, &n_read, error) || n_read != n)
        {
            if (!has_error(error))
            {
                set_error(error, EIO, "couldn't read file"sv);
            }
            ok = false;
            break;
        }

        sha.add(std::data(buffer), n);
        offset += n;
    }

    tr_sys_file_close(handle);

    if (ok)
    {
        out = tr_sha256_to_string(sha.finish()).sv();
    }

    return ok;
}

[[nodiscard]] bool remove_entry_files(std::string_view const dir, std::string_view const hash, tr_error* const error = nullptr)
{
    auto ok = true;
    for (auto const suffix : entry_suffixes())
    {
        auto const path = entry_filename(dir, hash, suffix);
        if (tr_sys_path_exists(path))
        {
            ok = tr_sys_path_remove(path, error) && ok;
        }
    }

    return ok;
}

[[nodiscard]] bool valid_archive_name(std::string_view const name)
{
    return tr::archive::is_valid_name(name);
}

[[nodiscard]] bool resolve_output_path(
    tr_session const& session,
    std::string_view const requested,
    std::string& result,
    tr_error* error)
{
    if (!std::empty(requested))
    {
        if (tr_sys_path_is_relative(requested))
        {
            result = fmt::format("{:s}/{:s}"sv, session.archiveExportsDir(), requested);
        }
        else
        {
            result = requested;
        }
    }
    else
    {
        result = fmt::format("{:s}/archive-{:d}.zip"sv, session.archiveExportsDir(), static_cast<int64_t>(std::time(nullptr)));
    }

    auto const parent = tr_sys_path_dirname(result);
    if (!std::empty(parent))
    {
        tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
        if (has_error(error))
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] std::string make_comment(std::string_view const type, std::string_view const hash, std::string_view const name)
{
    auto comment = fmt::format("Transmission archive {:s}; hash={:s}"sv, type, hash);
    if (!std::empty(name))
    {
        comment += fmt::format("; name={:s}"sv, name);
    }

    if (std::size(comment) > 4000U)
    {
        comment.resize(4000U);
    }

    return comment;
}

[[nodiscard]] bool is_windows_reserved_name(std::string_view const name)
{
    static constexpr auto ReservedNames = std::array<std::string_view, 22>{
        "con"sv,  "prn"sv,  "aux"sv,  "nul"sv,  "com1"sv, "com2"sv, "com3"sv, "com4"sv, "com5"sv, "com6"sv, "com7"sv,
        "com8"sv, "com9"sv, "lpt1"sv, "lpt2"sv, "lpt3"sv, "lpt4"sv, "lpt5"sv, "lpt6"sv, "lpt7"sv, "lpt8"sv, "lpt9"sv,
    };

    auto lower = std::string{ name };
    std::ranges::transform(lower, lower.begin(), [](unsigned char const ch) { return static_cast<char>(std::tolower(ch)); });

    auto const dot = lower.find('.');
    auto const base = lower.substr(0U, dot);
    return std::ranges::find(ReservedNames, base) != std::ranges::end(ReservedNames);
}

struct ManifestFile
{
    std::string name;
    std::string sha256;
};

struct ManifestTorrent
{
    tr::archive::Entry entry;
    std::vector<ManifestFile> files;
};

} // namespace

namespace tr::archive
{

class Cache
{
public:
    struct IndexEntry
    {
        std::string hash;
        bool has_torrent = false;
        bool has_resume = false;
        bool has_metadata = false;
        bool has_magnet = false;
    };

    struct Archive
    {
        bool index_valid = false;
        std::vector<IndexEntry> index;
        std::map<std::string, Entry> entries;
    };

    bool names_valid = false;
    std::vector<std::string> names;
    std::map<std::string, Archive> archives;
};

namespace
{

[[nodiscard]] Cache& get_cache(tr_session& session)
{
    auto& cache = session.archiveCache();
    if (!cache)
    {
        cache = std::make_shared<Cache>();
    }

    return *cache;
}

void scan_archive_names(tr_session const& session, std::vector<std::string>& names)
{
    names.clear();

    auto const dir = session.archiveDir();
    if (!tr_sys_path_exists(dir))
    {
        return;
    }

    auto found = std::set<std::string>{};
    for (auto const& name : tr_sys_dir_get_files(dir))
    {
        if (std::empty(name) || name.front() == '.' || name == "exports"sv || !valid_archive_name(name))
        {
            continue;
        }

        auto const info = tr_sys_path_get_info(archive_path(session, name));
        if (info && info->isFolder())
        {
            found.emplace(name);
        }
    }

    names.assign(std::begin(found), std::end(found));
}

[[nodiscard]] std::vector<Cache::IndexEntry> collect_index(std::string_view const dir)
{
    auto found = std::map<std::string, Cache::IndexEntry>{};

    for (auto const& name : tr_sys_dir_get_files(dir))
    {
        for (auto const suffix : entry_suffixes())
        {
            if (std::size(name) != 40U + std::size(suffix) || !name.ends_with(suffix))
            {
                continue;
            }

            auto const hash = name.substr(0U, 40U);
            if (!is_hash_string(hash))
            {
                continue;
            }

            auto const lower_hash = to_lower_hash(hash);
            auto& entry = found[lower_hash];
            entry.hash = lower_hash;

            if (suffix == ".torrent"sv)
            {
                entry.has_torrent = true;
            }
            else if (suffix == ".resume"sv)
            {
                entry.has_resume = true;
            }
            else if (suffix == ".json"sv)
            {
                entry.has_metadata = true;
            }
            else if (suffix == ".magnet"sv)
            {
                entry.has_magnet = true;
            }
        }
    }

    auto entries = std::vector<Cache::IndexEntry>{};
    entries.reserve(std::size(found));
    for (auto& [hash, entry] : found)
    {
        static_cast<void>(hash);
        entries.emplace_back(std::move(entry));
    }

    return entries;
}

[[nodiscard]] Cache::Archive& get_index(tr_session const& session, Cache& cache, std::string_view const name)
{
    auto& archive = cache.archives[std::string{ name }];
    if (!archive.index_valid)
    {
        auto const dir = archive_path(session, name);
        if (tr_sys_path_exists(dir))
        {
            archive.index = collect_index(dir);
        }

        archive.index_valid = true;
    }

    return archive;
}

[[nodiscard]] Entry const& get_entry(
    tr_session const& session,
    Cache::Archive& archive,
    std::string_view const archive_name,
    Cache::IndexEntry const& index_entry)
{
    auto const& hash = index_entry.hash;
    auto const hash_key = std::string{ hash };
    if (auto const iter = archive.entries.find(hash_key); iter != std::end(archive.entries))
    {
        return iter->second;
    }

    auto const dir = archive_path(session, archive_name);
    auto entry = Entry{};
    if (index_entry.has_metadata)
    {
        static_cast<void>(read_metadata(dir, hash, entry));
    }
    entry.archive = archive_name;
    entry.hash_string = hash;
    if (std::empty(entry.name))
    {
        entry.name = hash;
    }

    entry.has_torrent = index_entry.has_torrent;
    entry.has_resume = index_entry.has_resume;
    entry.has_metadata = index_entry.has_metadata;
    entry.has_magnet = index_entry.has_magnet;

    return archive.entries.emplace(hash_key, std::move(entry)).first->second;
}

void invalidate_names(tr_session& session)
{
    if (auto& cache = session.archiveCache(); cache)
    {
        cache->names_valid = false;
    }
}

void invalidate_archive(tr_session& session, std::string_view const name)
{
    if (auto& cache = session.archiveCache(); cache)
    {
        cache->archives.erase(std::string{ name });
    }
}

void forget_names(tr_session& session)
{
    auto const lock = std::unique_lock{ session.archiveMutex() };
    invalidate_names(session);
}

[[nodiscard]] int compare_strings_ci(std::string_view const lhs, std::string_view const rhs)
{
    auto const lhs_lower = tr_strlower(lhs);
    auto const rhs_lower = tr_strlower(rhs);
    return lhs_lower < rhs_lower ? -1 : (rhs_lower < lhs_lower ? 1 : 0);
}

template<typename T>
[[nodiscard]] int compare_values(T const& lhs, T const& rhs)
{
    return lhs < rhs ? -1 : (rhs < lhs ? 1 : 0);
}

[[nodiscard]] int compare_entries(Entry const& lhs, Entry const& rhs, SortBy const sort_by)
{
    switch (sort_by)
    {
    case SortBy::Name:
        return compare_strings_ci(lhs.name, rhs.name);

    case SortBy::TotalSize:
        return compare_values(lhs.total_size, rhs.total_size);

    case SortBy::AddedDate:
        return compare_values(lhs.added_date, rhs.added_date);

    case SortBy::ArchivedDate:
        return compare_values(lhs.archived_date, rhs.archived_date);

    case SortBy::DownloadDir:
        return compare_strings_ci(lhs.download_dir, rhs.download_dir);

    case SortBy::None:
        break;
    }

    return 0;
}

[[nodiscard]] bool entry_comes_before(Entry const& lhs, Entry const& rhs, ListRequest const& request)
{
    auto cmp = compare_entries(lhs, rhs, request.sort_by);
    if (cmp == 0)
    {
        cmp = compare_strings_ci(lhs.name, rhs.name);
    }
    if (cmp == 0)
    {
        cmp = compare_strings_ci(lhs.archive, rhs.archive);
    }
    if (cmp == 0)
    {
        cmp = compare_values(lhs.hash_string, rhs.hash_string);
    }

    return request.sort_order == SortOrder::Descending ? cmp > 0 : cmp < 0;
}

} // namespace

bool is_valid_name(std::string_view const name)
{
    if (std::empty(name) || std::size(name) > 128U || name == "."sv || name == ".."sv || name == "exports"sv ||
        name == AllArchives || name.back() == '.' || name.back() == ' ' || is_windows_reserved_name(name))
    {
        return false;
    }

    for (auto const ch : name)
    {
        auto const uch = static_cast<unsigned char>(ch);
        if (uch < 0x20U || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|')
        {
            return false;
        }
    }

    return true;
}

std::string archive_path(tr_session const& session, std::string_view const archive_name)
{
    return fmt::format("{:s}/{:s}"sv, session.archiveDir(), archive_name);
}

std::string export_path(tr_session const& session, std::string_view const archive_name, std::string_view const filename)
{
    return fmt::format("{:s}/{:s}"sv, session.archiveExportsDir(), std::empty(filename) ? archive_name : filename);
}

bool list_archives(tr_session& session, std::vector<std::string>& names, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    auto const lock = std::unique_lock{ session.archiveMutex() };
    auto& cache = get_cache(session);
    if (!cache.names_valid)
    {
        scan_archive_names(session, cache.names);
        cache.names_valid = true;
    }

    names = cache.names;
    return true;
}

bool create_archive(tr_session& session, std::string_view const archive_name, bool& created, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    created = false;

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const lock = std::unique_lock{ session.archiveMutex() };
    auto const dir = archive_path(session, archive_name);

    if (tr_sys_path_exists(dir))
    {
        auto const info = tr_sys_path_get_info(dir);
        if (!info || !info->isFolder())
        {
            set_error(error, EEXIST, "archive path exists and is not a directory"sv);
            return false;
        }

        return true;
    }

    tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
    if (has_error(error))
    {
        return false;
    }

    created = true;
    invalidate_names(session);
    tr_logAddInfo(fmt::format("Created archive '{}'", archive_name));
    return true;
}

bool delete_archive(tr_session& session, std::string_view const archive_name, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const lock = std::unique_lock{ session.archiveMutex() };
    auto const dir = archive_path(session, archive_name);
    if (!tr_sys_path_exists(dir))
    {
        set_error(error, ENOENT, "archive does not exist"sv);
        return false;
    }

    auto const info = tr_sys_path_get_info(dir);
    if (!info || !info->isFolder())
    {
        set_error(error, ENOENT, "archive does not exist"sv);
        return false;
    }

    if (!std::empty(tr_sys_dir_get_files(dir)))
    {
        set_error(error, EEXIST, "archive is not empty"sv);
        return false;
    }

    if (!tr_sys_path_remove(dir, error))
    {
        return false;
    }

    invalidate_names(session);
    invalidate_archive(session, archive_name);
    tr_logAddInfo(fmt::format("Deleted empty archive '{}'", archive_name));
    return true;
}

bool delete_entry(
    tr_session& session,
    std::string_view const archive_name,
    std::string_view const hash_string,
    DeleteResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    if (!is_hash_string(hash_string))
    {
        set_error(error, EINVAL, "invalid info hash"sv);
        return false;
    }

    auto const lock = std::unique_lock{ session.archiveMutex() };
    auto const dir = archive_path(session, archive_name);
    if (!tr_sys_path_exists(dir))
    {
        set_error(error, ENOENT, "archive does not exist"sv);
        return false;
    }

    auto const hash = to_lower_hash(hash_string);
    if (!has_any_entry(dir, hash))
    {
        set_error(error, ENOENT, "archive entry does not exist"sv);
        return false;
    }

    auto ok = true;
    auto files_removed = size_t{};
    for (auto const suffix : entry_suffixes())
    {
        auto const path = entry_filename(dir, hash, suffix);
        if (tr_sys_path_exists(path))
        {
            if (tr_sys_path_remove(path, error))
            {
                ++files_removed;
            }
            else
            {
                ok = false;
            }
        }
    }

    invalidate_archive(session, archive_name);

    if (!ok)
    {
        return false;
    }

    result.archive = archive_name;
    result.hash_string = hash;
    result.files_removed = files_removed;

    tr_logAddInfo(fmt::format("Deleted {} file(s) for torrent {} from archive '{}'", files_removed, hash, archive_name));
    return true;
}

bool list(
    tr_session& session,
    std::string_view const archive_name,
    ListRequest const& request,
    ListResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    auto const list_all = std::empty(archive_name) || archive_name == AllArchives;
    if (!list_all && !valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const lock = std::unique_lock{ session.archiveMutex() };
    auto& cache = get_cache(session);

    auto single_name = std::vector<std::string>{};
    auto const* names = static_cast<std::vector<std::string> const*>(nullptr);
    if (list_all)
    {
        if (!cache.names_valid)
        {
            scan_archive_names(session, cache.names);
            cache.names_valid = true;
        }

        names = &cache.names;
    }
    else
    {
        single_name.emplace_back(archive_name);
        names = &single_name;
    }

    auto total = size_t{};
    for (auto const& name : *names)
    {
        total += get_index(session, cache, name).index.size();
    }

    if (request.sort_by != SortBy::None || !std::empty(request.label))
    {
        auto entries = std::vector<Entry>{};
        entries.reserve(total);
        for (auto const& name : *names)
        {
            auto& archive = get_index(session, cache, name);
            for (auto const& index_entry : archive.index)
            {
                entries.emplace_back(get_entry(session, archive, name, index_entry));
            }
        }

        if (!std::empty(request.label))
        {
            std::erase_if(
                entries,
                [&request](Entry const& entry)
                { return std::ranges::find(entry.labels, request.label) == std::ranges::end(entry.labels); });
        }

        if (request.sort_by != SortBy::None)
        {
            std::ranges::stable_sort(
                entries,
                [&request](Entry const& lhs, Entry const& rhs) { return entry_comes_before(lhs, rhs, request); });
        }

        result.total = std::size(entries);
        auto const begin = std::min(request.offset, result.total);
        auto const available = result.total - begin;
        auto const count = request.limit == 0U ? available : std::min(request.limit, available);
        result.entries.reserve(count);
        for (auto i = begin; i < begin + count; ++i)
        {
            result.entries.emplace_back(std::move(entries[i]));
        }

        return true;
    }

    result.total = total;

    auto const begin = std::min(request.offset, total);
    auto const available = total - begin;
    auto const count = request.limit == 0U ? available : std::min(request.limit, available);
    auto const end = begin + count;
    if (begin == end)
    {
        return true;
    }

    result.entries.reserve(count);

    auto seen = size_t{};
    for (auto const& name : *names)
    {
        auto& archive = get_index(session, cache, name);
        for (auto const& entry : archive.index)
        {
            if (seen >= end)
            {
                break;
            }

            if (seen >= begin)
            {
                result.entries.emplace_back(get_entry(session, archive, name, entry));
            }

            ++seen;
        }

        if (seen >= end)
        {
            break;
        }
    }

    return true;
}

bool save(
    tr_session& session,
    tr_torrent& tor,
    std::string_view const archive_name,
    bool const delete_local_data,
    bool const overwrite,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const dir = archive_path(session, archive_name);
    tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
    if (has_error(error))
    {
        return false;
    }

    forget_names(session);

    auto const hash = std::string{ tor.info_hash_string().sv() };
    if (!is_hash_string(hash))
    {
        set_error(error, EINVAL, "torrent has an invalid info hash"sv);
        return false;
    }

    if (!overwrite && has_any_entry(dir, hash))
    {
        set_error(error, EEXIST, "archive already contains this torrent"sv);
        return false;
    }

    tr_logAddInfo(fmt::format("Archiving torrent '{}' to archive '{}'", tor.name(), archive_name));

    tor.stop_and_save_resume();

    auto const has_torrent = tor.has_metainfo();
    auto const has_magnet = !has_torrent && tr_sys_path_exists(tor.magnet_file());
    if (!has_torrent && !has_magnet)
    {
        set_error(error, ENOENT, "torrent has neither metainfo nor a magnet link"sv);
        return false;
    }

    auto entry = Entry{};
    entry.hash_string = hash;
    entry.name = tor.name();
    entry.download_dir = tor.download_dir().sv();
    entry.added_date = static_cast<int64_t>(tor.date_added());
    entry.archived_date = static_cast<int64_t>(std::time(nullptr));
    entry.total_size = tor.total_size();
    entry.local_data_deleted = delete_local_data;
    entry.has_torrent = has_torrent;
    entry.has_magnet = has_magnet;

    for (auto const& label : tor.labels())
    {
        entry.labels.emplace_back(label.sv());
    }

    auto stage = ScratchDir{};
    stage.path = make_temp_dir(dir, ".save-"sv, error);
    if (std::empty(stage.path))
    {
        return false;
    }

    auto const stage_path = [&stage, &hash](std::string_view const suffix)
    {
        auto path = entry_filename(stage.path, hash, suffix);
        stage.files.emplace_back(path);
        return path;
    };

    if (has_torrent && !copy_file_atomic(tor.torrent_file(), stage_path(".torrent"sv), error))
    {
        return false;
    }

    if (has_magnet && !copy_file_atomic(tor.magnet_file(), stage_path(".magnet"sv), error))
    {
        return false;
    }

    auto const resume_source = std::string{ tor.resume_file() };
    if (tr_sys_path_exists(resume_source))
    {
        if (!copy_file_atomic(resume_source, stage_path(".resume"sv), error))
        {
            return false;
        }

        entry.has_resume = true;
    }

    if (!tr_file_save(stage_path(".json"sv), make_metadata_json(entry), error))
    {
        return false;
    }

    auto lock = std::unique_lock{ session.archiveMutex() };

    invalidate_names(session);
    invalidate_archive(session, archive_name);

    if (!overwrite && has_any_entry(dir, hash))
    {
        set_error(error, EEXIST, "archive already contains this torrent"sv);
        return false;
    }

    auto backup = ScratchDir{};
    backup.path = make_temp_dir(dir, ".save-backup-"sv, error);
    if (std::empty(backup.path))
    {
        return false;
    }

    auto backup_pairs = std::vector<std::pair<std::string, std::string>>{};
    auto const restore_backup = [&backup_pairs]()
    {
        for (auto iter = std::rbegin(backup_pairs); iter != std::rend(backup_pairs); ++iter)
        {
            tr_sys_path_rename(iter->first, iter->second, nullptr);
        }
    };

    for (auto const suffix : entry_suffixes())
    {
        auto const final_path = entry_filename(dir, hash, suffix);
        if (!tr_sys_path_exists(final_path))
        {
            continue;
        }

        auto backup_path = entry_filename(backup.path, hash, suffix);
        backup.files.emplace_back(backup_path);
        if (!tr_sys_path_rename(final_path, backup_path, error))
        {
            restore_backup();
            return false;
        }

        backup_pairs.emplace_back(std::move(backup_path), final_path);
    }

    auto installed = std::vector<std::string>{};
    for (auto const suffix : entry_suffixes())
    {
        auto const staged_path = entry_filename(stage.path, hash, suffix);
        if (!tr_sys_path_exists(staged_path))
        {
            continue;
        }

        auto const final_path = entry_filename(dir, hash, suffix);
        if (!tr_sys_path_rename(staged_path, final_path, error))
        {
            for (auto const& path : installed)
            {
                tr_sys_path_remove(path, nullptr);
            }

            restore_backup();
            return false;
        }

        installed.emplace_back(final_path);
    }

    for (auto const& [backup_path, final_path] : backup_pairs)
    {
        static_cast<void>(final_path);
        tr_sys_path_remove(backup_path, nullptr);
    }

    backup.files.clear();

    lock.unlock();

    tr_torrentRemove(&tor, delete_local_data);
    tr_logAddInfo(fmt::format("Archived torrent '{}' in archive '{}'", entry.name, archive_name));
    return true;
}
bool restore(
    tr_session& session,
    std::string_view const archive_name,
    std::string_view const hash_string,
    std::string_view const requested_download_dir,
    bool const remove_from_archive,
    bool const verify,
    RestoreResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    if (!is_hash_string(hash_string))
    {
        set_error(error, EINVAL, "invalid info hash"sv);
        return false;
    }

    auto const hash = to_lower_hash(hash_string);
    if (session.torrents().get(hash) != nullptr)
    {
        set_error(error, EEXIST, "torrent is already in the session"sv);
        return false;
    }

    auto const dir = archive_path(session, archive_name);
    auto const torrent_source = entry_filename(dir, hash, ".torrent"sv);
    auto const magnet_source = entry_filename(dir, hash, ".magnet"sv);
    auto const has_torrent = tr_sys_path_exists(torrent_source);
    auto const has_magnet = tr_sys_path_exists(magnet_source);
    if (!has_torrent && !has_magnet)
    {
        set_error(error, ENOENT, "archive entry has neither a torrent file nor a magnet link"sv);
        return false;
    }

    if (!std::empty(requested_download_dir) && tr_sys_path_is_relative(requested_download_dir))
    {
        set_error(error, EINVAL, "download directory path is not absolute"sv);
        return false;
    }

    auto lock = std::unique_lock{ session.archiveMutex() };

    auto ctor = tr_ctor{ &session };
    if (has_torrent)
    {
        if (!ctor.set_metainfo_from_file(torrent_source, error))
        {
            return false;
        }
    }
    else
    {
        auto magnet_contents = std::vector<char>{};
        if (!tr_file_read(magnet_source, magnet_contents, error))
        {
            return false;
        }

        if (!ctor.set_metainfo_from_magnet_link(
                std::string_view{ std::data(magnet_contents), std::size(magnet_contents) },
                error))
        {
            return false;
        }
    }

    auto const actual_hash = to_lower_hash(ctor.metainfo().info_hash_string().sv());
    if (actual_hash != hash)
    {
        set_error(error, EINVAL, "archive entry does not match the requested info hash"sv);
        return false;
    }

    auto entry = Entry{};
    static_cast<void>(read_metadata(dir, hash, entry));

    auto download_dir = std::string{ requested_download_dir };
    if (std::empty(download_dir))
    {
        download_dir = entry.download_dir;
    }
    if (std::empty(download_dir))
    {
        download_dir = session.downloadDir();
    }

    ctor.set_download_dir(TR_FORCE, download_dir);

    if (!std::empty(entry.labels))
    {
        auto labels = tr_torrent::labels_t{};
        labels.reserve(std::size(entry.labels));
        for (auto const& label : entry.labels)
        {
            labels.emplace_back(label);
        }
        ctor.set_labels(std::move(labels));
    }

    auto const torrent_dest = fmt::format("{:s}/{:s}.torrent"sv, session.torrentDir(), hash);
    auto const magnet_dest = fmt::format("{:s}/{:s}.magnet"sv, session.torrentDir(), hash);
    auto const resume_dest = fmt::format("{:s}/{:s}.resume"sv, session.resumeDir(), hash);

    for (auto const& path : { torrent_dest, magnet_dest, resume_dest })
    {
        if (tr_sys_path_exists(path) && !tr_sys_path_remove(path, error))
        {
            return false;
        }
    }

    auto const resume_source = entry_filename(dir, hash, ".resume"sv);
    if (tr_sys_path_exists(resume_source) && !copy_file_atomic(resume_source, resume_dest, error))
    {
        return false;
    }

    auto const cleanup_session_files = [&]()
    {
        tr_sys_path_remove(torrent_dest, nullptr);
        tr_sys_path_remove(magnet_dest, nullptr);
        tr_sys_path_remove(resume_dest, nullptr);
    };

    auto* duplicate_of = static_cast<tr_torrent*>(nullptr);
    auto* const tor = tr_torrentNew(&ctor, &duplicate_of);
    if (tor == nullptr)
    {
        cleanup_session_files();
        if (duplicate_of != nullptr)
        {
            set_error(error, EEXIST, "torrent is already in the session"sv);
        }
        else if (!has_error(error))
        {
            set_error(error, EINVAL, "couldn't add torrent from archive"sv);
        }
        return false;
    }

    if (verify)
    {
        tr_torrentVerify(tor);
    }

    session.rpcNotify(TR_RPC_TORRENT_ADDED, tor->id());
    result.archive = archive_name;
    result.hash_string = hash;

    if (remove_from_archive)
    {
        auto remove_error = tr_error{};
        result.removed = remove_entry_files(dir, hash, &remove_error);
        if (!result.removed)
        {
            result.remove_error = remove_error.message();
            tr_logAddWarn(fmt::format("Restored torrent '{}' but could not remove it from archive '{}'", hash, archive_name));
        }

        invalidate_archive(session, archive_name);
    }

    tr_logAddInfo(fmt::format("Restored torrent '{}' from archive '{}'", hash, archive_name));
    return true;
}

bool restore(tr_session& session, std::vector<RestoreRequest> const& requests, RestoreBatchResult& result, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};
    if (std::empty(requests))
    {
        set_error(error, EINVAL, "no restore items"sv);
        return false;
    }

    for (auto const& request : requests)
    {
        auto item_result = RestoreResult{};
        auto item_error = tr_error{};
        if (restore(
                session,
                request.archive,
                request.hash_string,
                request.download_dir,
                request.remove_from_archive,
                request.verify,
                item_result,
                &item_error))
        {
            result.entries.emplace_back(std::move(item_result));
            ++result.restored;
        }
        else
        {
            result.errors.emplace_back(
                RestoreError{ request.archive, request.hash_string, std::string{ item_error.message() } });
        }
    }

    return true;
}

bool move(
    tr_session& session,
    std::string_view const source_archive,
    std::string_view const target_archive,
    std::vector<std::string> const& requested_hashes,
    bool const overwrite,
    MoveResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(source_archive) || !valid_archive_name(target_archive))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    if (source_archive == target_archive)
    {
        set_error(error, EINVAL, "source and target archives are the same"sv);
        return false;
    }

    auto const source_dir = archive_path(session, source_archive);
    auto const target_dir = archive_path(session, target_archive);
    if (!tr_sys_path_exists(source_dir))
    {
        set_error(error, ENOENT, "source archive does not exist"sv);
        return false;
    }

    tr_sys_dir_create(target_dir, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
    if (has_error(error))
    {
        return false;
    }

    auto lock = std::unique_lock{ session.archiveMutex() };
    invalidate_names(session);
    invalidate_archive(session, source_archive);
    invalidate_archive(session, target_archive);

    auto hashes = std::vector<std::string>{};
    if (std::empty(requested_hashes))
    {
        hashes = collect_hashes(source_dir);
    }
    else
    {
        auto unique_hashes = std::set<std::string>{};
        for (auto const& hash : requested_hashes)
        {
            if (!is_hash_string(hash))
            {
                set_error(error, EINVAL, "invalid info hash"sv);
                return false;
            }
            unique_hashes.emplace(to_lower_hash(hash));
        }
        hashes.assign(std::begin(unique_hashes), std::end(unique_hashes));
    }

    for (auto const& hash : hashes)
    {
        if (!has_any_entry(source_dir, hash))
        {
            result.missing.emplace_back(hash);
            continue;
        }

        if (!overwrite && has_any_entry(target_dir, hash))
        {
            set_error(error, EEXIST, "target archive already contains this torrent"sv);
            return false;
        }
    }

    for (auto const& hash : hashes)
    {
        if (!has_any_entry(source_dir, hash))
        {
            continue;
        }

        if (overwrite)
        {
            if (!remove_entry_files(target_dir, hash, error))
            {
                return false;
            }
        }

        auto moved_hash = false;
        for (auto const suffix : entry_suffixes())
        {
            auto const source = entry_filename(source_dir, hash, suffix);
            if (!tr_sys_path_exists(source))
            {
                continue;
            }

            auto const target = entry_filename(target_dir, hash, suffix);
            if (!tr_file_move(source, target, true, error))
            {
                return false;
            }

            moved_hash = true;
        }

        if (moved_hash)
        {
            ++result.moved;
        }
    }

    tr_logAddInfo(
        fmt::format(
            "Moved {} torrent(s) from archive '{}' to '{}' ({} missing)",
            result.moved,
            source_archive,
            target_archive,
            result.missing.size()));
    return true;
}
bool export_bundle(
    tr_session& session,
    std::string_view const archive_name,
    std::vector<std::string> const& requested_hashes,
    std::string_view const requested_output_path,
    bool const overwrite,
    ExportResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const dir = archive_path(session, archive_name);
    if (!tr_sys_path_exists(dir))
    {
        set_error(error, ENOENT, "archive does not exist"sv);
        return false;
    }

    if (!resolve_output_path(session, requested_output_path, result.path, error))
    {
        return false;
    }

    if (!overwrite && tr_sys_path_exists(result.path))
    {
        set_error(error, EEXIST, "export file already exists"sv);
        return false;
    }

    auto lock = std::shared_lock{ session.archiveMutex() };

    auto hashes = std::vector<std::string>{};
    if (std::empty(requested_hashes))
    {
        hashes = collect_hashes(dir);
    }
    else
    {
        auto unique_hashes = std::set<std::string>{};
        for (auto const& hash : requested_hashes)
        {
            if (!is_hash_string(hash))
            {
                set_error(error, EINVAL, "invalid info hash"sv);
                return false;
            }
            unique_hashes.emplace(to_lower_hash(hash));
        }
        hashes.assign(std::begin(unique_hashes), std::end(unique_hashes));
    }

    auto sources = std::vector<BundleSource>{};
    auto manifest_torrents = tr_variant::make_vector(hashes.size());
    auto* const manifest_vec = manifest_torrents.get_if<tr_variant::Vector>();

    for (auto const& hash : hashes)
    {
        auto entry = Entry{};
        static_cast<void>(read_metadata(dir, hash, entry));
        entry.hash_string = hash;
        if (std::empty(entry.name))
        {
            entry.name = hash;
        }

        auto file_vars = tr_variant::make_vector(4U);
        auto* const file_vec = file_vars.get_if<tr_variant::Vector>();
        auto added = false;

        for (auto const suffix : entry_suffixes())
        {
            auto const path = entry_filename(dir, hash, suffix);
            if (!tr_sys_path_exists(path))
            {
                continue;
            }

            auto const filename = fmt::format("{:s}{:s}"sv, hash, suffix);
            auto sha256 = std::string{};
            if (sha256_file(path, sha256, error))
            {
                auto file_map = tr_variant::Map{};
                file_map.try_emplace(key("name"), filename);
                file_map.try_emplace(key("sha256"), sha256);
                file_vec->emplace_back(std::move(file_map));
            }
            else if (has_error(error))
            {
                return false;
            }

            auto type = std::string_view{ "file"sv };
            if (suffix == ".torrent"sv)
            {
                type = "torrent metainfo"sv;
            }
            else if (suffix == ".resume"sv)
            {
                type = "resume data"sv;
            }
            else if (suffix == ".json"sv)
            {
                type = "archive metadata"sv;
            }
            else if (suffix == ".magnet"sv)
            {
                type = "magnet link"sv;
            }

            sources.emplace_back(
                BundleSource{
                    filename,
                    make_comment(type, hash, entry.name),
                    path,
                    {},
                });
            added = true;
        }

        if (!added)
        {
            continue;
        }

        auto map = tr_variant::Map{};
        map.try_emplace(key("hash_string"), hash);
        map.try_emplace(key("name"), entry.name);
        map.try_emplace(key("download_dir"), entry.download_dir);
        map.try_emplace(key("archived_date"), entry.archived_date);
        map.try_emplace(key("total_size"), static_cast<int64_t>(entry.total_size));
        map.try_emplace(key("local_data_deleted"), entry.local_data_deleted);
        map.try_emplace(key("files"), std::move(file_vars));
        manifest_vec->emplace_back(std::move(map));
        ++result.count;
    }

    if (std::empty(sources))
    {
        set_error(error, ENOENT, "archive has no matching torrents"sv);
        return false;
    }

    auto manifest_map = tr_variant::Map{};
    manifest_map.try_emplace(key("format"), "transmission-archive"sv);
    manifest_map.try_emplace(key("version"), 1);
    manifest_map.try_emplace(key("created_date"), static_cast<int64_t>(std::time(nullptr)));
    manifest_map.try_emplace(key("source_archive"), archive_name);
    manifest_map.try_emplace(key("torrents"), std::move(manifest_torrents));
    auto const manifest_root = tr_variant{ std::move(manifest_map) };
    auto const manifest = tr_variant_serde::json().compact().to_string(manifest_root);
    if (std::size(manifest) > MaxManifestBytes)
    {
        set_error(error, EINVAL, "archive manifest is too large for one bundle"sv);
        return false;
    }

    sources.insert(
        std::begin(sources),
        BundleSource{
            std::string{ BundleManifestFilename },
            std::string{ "Transmission archive manifest; format=transmission-archive; version=1"sv },
            {},
            manifest,
        });

    if (std::size(sources) > MaxBundleEntries)
    {
        set_error(error, EINVAL, "too many archive entries for one export"sv);
        return false;
    }

    if (!write_bundle(result.path, sources, error))
    {
        return false;
    }

    lock.unlock();

    auto const info = tr_sys_path_get_info(result.path, 0, error);
    if (!info)
    {
        return false;
    }

    result.size = info->size;
    if (!sha256_file(result.path, result.sha256, error))
    {
        return false;
    }

    tr_logAddInfo(fmt::format("Exported {} torrent(s) from archive '{}' to '{}'", result.count, archive_name, result.path));
    return true;
}

bool view(
    tr_session& session,
    std::string_view const archive_name,
    std::string_view const hash_string,
    ViewResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    if (!is_hash_string(hash_string))
    {
        set_error(error, EINVAL, "invalid info hash"sv);
        return false;
    }

    auto const lock = std::shared_lock{ session.archiveMutex() };
    auto const dir = archive_path(session, archive_name);
    if (!tr_sys_path_exists(dir))
    {
        set_error(error, ENOENT, "archive does not exist"sv);
        return false;
    }

    auto const hash = to_lower_hash(hash_string);
    auto entry = Entry{};
    static_cast<void>(read_metadata(dir, hash, entry));
    entry.hash_string = hash;
    if (std::empty(entry.name))
    {
        entry.name = hash;
    }

    auto sources = std::vector<BundleSource>{};
    auto files = std::vector<std::string>{};

    for (auto const suffix : { ".torrent"sv, ".resume"sv })
    {
        auto const path = entry_filename(dir, hash, suffix);
        if (!tr_sys_path_exists(path))
        {
            continue;
        }

        auto const type = suffix == ".torrent"sv ? "torrent metainfo"sv : "resume data"sv;
        auto const filename = fmt::format("{:s}{:s}"sv, hash, suffix);
        sources.emplace_back(
            BundleSource{
                filename,
                make_comment(type, hash, entry.name),
                path,
                {},
            });
        files.emplace_back(filename);
    }

    if (std::empty(sources))
    {
        set_error(error, ENOENT, "archive entry has no torrent or resume file"sv);
        return false;
    }

    auto const exports_dir = std::string{ session.archiveExportsDir() };
    tr_sys_dir_create(exports_dir, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
    if (has_error(error))
    {
        return false;
    }

    auto temp = ScratchDir{};
    temp.path = make_temp_dir(exports_dir, ".view-"sv, error);
    if (std::empty(temp.path))
    {
        if (!has_error(error))
        {
            set_error(error, EIO, "couldn't create temporary directory"sv);
        }
        return false;
    }

    auto const zip_path = fmt::format("{:s}/{:s}.zip"sv, temp.path, hash);
    temp.files.emplace_back(zip_path);
    if (!write_bundle(zip_path, sources, error))
    {
        return false;
    }

    auto contents = std::vector<char>{};
    if (!tr_file_read(zip_path, contents, error))
    {
        return false;
    }

    result.archive = archive_name;
    result.hash_string = hash;
    result.name = entry.name;
    result.filename = fmt::format("{:s}.zip"sv, hash);
    result.files = std::move(files);
    result.size = std::size(contents);
    result.data.assign(std::data(contents), std::size(contents));

    tr_logAddInfo(
        fmt::format(
            "Created view bundle for torrent {} from archive '{}' ({} file(s), {} bytes)",
            hash,
            archive_name,
            result.files.size(),
            result.size));
    return true;
}

bool import_bundle(
    tr_session& session,
    std::string_view const bundle_path,
    std::string_view const archive_name,
    bool const overwrite,
    ImportResult& result,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    result = {};

    if (!valid_archive_name(archive_name))
    {
        set_error(error, EINVAL, "invalid archive name"sv);
        return false;
    }

    auto const resolved_bundle_path = tr_sys_path_is_relative(bundle_path) ?
        fmt::format("{:s}/{:s}"sv, session.archiveExportsDir(), bundle_path) :
        std::string{ bundle_path };

    auto entries = std::vector<BundleEntryInfo>{};
    if (!read_bundle_index(resolved_bundle_path, entries, error))
    {
        return false;
    }

    auto const* const manifest_info = find_entry(entries, BundleManifestFilename);
    if (manifest_info == nullptr)
    {
        set_error(error, EINVAL, "bundle has no manifest"sv);
        return false;
    }

    auto manifest_contents = std::string{};
    if (!read_bundle_entry(resolved_bundle_path, *manifest_info, manifest_contents, MaxManifestBytes, error))
    {
        return false;
    }

    auto parsed = tr_variant_serde::json().parse(manifest_contents);
    if (!parsed)
    {
        set_error(error, EINVAL, "bundle manifest is not valid JSON"sv);
        return false;
    }

    auto const* const manifest = parsed->get_if<tr_variant::Map>();
    if (manifest == nullptr || manifest->value_if<std::string_view>(key("format")) != "transmission-archive"sv ||
        manifest->value_if<int64_t>(key("version")) != 1)
    {
        set_error(error, EINVAL, "unsupported bundle manifest"sv);
        return false;
    }

    auto manifest_entries = std::map<std::string, ManifestTorrent>{};
    if (auto const* const torrents = manifest->find_if<tr_variant::Vector>(key("torrents")); torrents != nullptr)
    {
        for (auto const& torrent_var : *torrents)
        {
            auto const* const map = torrent_var.get_if<tr_variant::Map>();
            if (map == nullptr)
            {
                continue;
            }

            auto const hash = map->value_if<std::string_view>(key("hash_string")).value_or(""sv);
            if (!is_hash_string(hash))
            {
                continue;
            }

            auto const lower_hash = to_lower_hash(hash);
            if (manifest_entries.contains(lower_hash))
            {
                set_error(error, EINVAL, "bundle manifest contains duplicate torrents"sv);
                return false;
            }

            auto& info = manifest_entries[lower_hash];
            info.entry.hash_string = lower_hash;
            info.entry.name = map->value_if<std::string_view>(key("name")).value_or(info.entry.hash_string);
            info.entry.download_dir = map->value_if<std::string_view>(key("download_dir")).value_or(""sv);
            info.entry.archived_date = map->value_if<int64_t>(key("archived_date")).value_or(0);
            info.entry.total_size = map->value_if<int64_t>(key("total_size")).value_or(0);
            info.entry.local_data_deleted = map->value_if<bool>(key("local_data_deleted")).value_or(false);

            if (auto const* const files = map->find_if<tr_variant::Vector>(key("files")); files != nullptr)
            {
                for (auto const& file_var : *files)
                {
                    auto const* const file_map = file_var.get_if<tr_variant::Map>();
                    if (file_map == nullptr)
                    {
                        continue;
                    }

                    auto const name = file_map->value_if<std::string_view>(key("name")).value_or(""sv);
                    auto const sha256 = file_map->value_if<std::string_view>(key("sha256")).value_or(""sv);
                    if (!is_bundle_entry_name(name) || !name.starts_with(lower_hash + ".") || std::size(sha256) != 64U)
                    {
                        set_error(error, EINVAL, "bundle manifest contains an invalid file entry"sv);
                        return false;
                    }

                    info.files.push_back(ManifestFile{ std::string{ name }, std::string{ sha256 } });
                }
            }
        }
    }

    auto const target_dir = archive_path(session, archive_name);
    tr_sys_dir_create(target_dir, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
    if (has_error(error))
    {
        return false;
    }

    forget_names(session);

    auto temp_dir = tr_pathbuf{ target_dir, "/.import.XXXXXX"sv };
    if (!tr_sys_dir_create_temp(std::data(temp_dir), error))
    {
        return false;
    }

    auto import_temp = ScratchDir{};
    import_temp.path = temp_dir.sv();

    auto total_extract_size = uint64_t{};
    for (auto const& entry : entries)
    {
        if (entry.name == BundleManifestFilename)
        {
            continue;
        }

        if (entry.size > MaxBundleBytes - total_extract_size)
        {
            set_error(error, EINVAL, "bundle extraction is too large"sv);
            return false;
        }

        total_extract_size += entry.size;

        auto const destination = fmt::format("{:s}/{:s}"sv, import_temp.path, entry.name);
        if (!extract_bundle_entry(resolved_bundle_path, entry, destination, error))
        {
            return false;
        }

        import_temp.files.emplace_back(destination);
    }

    auto expected_files = std::map<std::string, std::string, std::less<>>{};
    for (auto const& [hash, torrent] : manifest_entries)
    {
        static_cast<void>(hash);
        for (auto const& file : torrent.files)
        {
            if (!expected_files.emplace(file.name, file.sha256).second)
            {
                set_error(error, EINVAL, "bundle manifest contains duplicate files"sv);
                return false;
            }
        }
    }

    if (std::empty(expected_files))
    {
        set_error(error, EINVAL, "bundle contains no torrent files"sv);
        return false;
    }

    auto extracted_names = std::set<std::string, std::less<>>{};
    for (auto const& path : import_temp.files)
    {
        extracted_names.emplace(std::string{ tr_sys_path_basename(path) });
    }

    if (extracted_names.size() != expected_files.size())
    {
        set_error(error, EINVAL, "bundle files do not match the manifest"sv);
        return false;
    }

    for (auto const& path : import_temp.files)
    {
        auto const filename = std::string{ tr_sys_path_basename(path) };
        auto const expected = expected_files.find(filename);
        if (expected == std::end(expected_files))
        {
            set_error(error, EINVAL, "bundle contains a file that is not in the manifest"sv);
            return false;
        }

        auto sha256 = std::string{};
        if (!sha256_file(path, sha256, error))
        {
            return false;
        }

        if (sha256 != expected->second)
        {
            set_error(error, EINVAL, "bundle file does not match its manifest checksum"sv);
            return false;
        }
    }

    auto lock = std::unique_lock{ session.archiveMutex() };
    invalidate_names(session);
    invalidate_archive(session, archive_name);

    auto hashes = std::set<std::string>{};

    for (auto const& temp_path : import_temp.files)
    {
        auto const filename = std::string{ tr_sys_path_basename(temp_path) };
        auto const dot = filename.rfind('.');
        hashes.emplace(to_lower_hash(filename.substr(0U, dot)));
    }

    for (auto const& hash : hashes)
    {
        if (!overwrite && has_any_entry(target_dir, hash))
        {
            ++result.skipped;
            continue;
        }

        if (overwrite)
        {
            if (!remove_entry_files(target_dir, hash, error))
            {
                return false;
            }
        }

        for (auto const suffix : entry_suffixes())
        {
            auto const source = entry_filename(import_temp.path, hash, suffix);
            if (!tr_sys_path_exists(source))
            {
                continue;
            }

            auto const destination = entry_filename(target_dir, hash, suffix);
            if (!tr_file_move(source, destination, true, error))
            {
                return false;
            }
        }

        if (!tr_sys_path_exists(entry_filename(target_dir, hash, ".json"sv)))
        {
            if (auto const iter = manifest_entries.find(hash); iter != std::end(manifest_entries))
            {
                auto entry = iter->second.entry;
                entry.has_torrent = tr_sys_path_exists(entry_filename(target_dir, hash, ".torrent"sv));
                entry.has_resume = tr_sys_path_exists(entry_filename(target_dir, hash, ".resume"sv));
                entry.has_magnet = tr_sys_path_exists(entry_filename(target_dir, hash, ".magnet"sv));
                if (!tr_file_save(entry_filename(target_dir, hash, ".json"sv), make_metadata_json(entry), error))
                {
                    return false;
                }
            }
        }

        result.hashes.emplace_back(hash);
    }

    tr_logAddInfo(
        fmt::format(
            "Imported {} torrent(s) into archive '{}' ({} skipped)",
            result.hashes.size(),
            archive_name,
            result.skipped));
    return true;
}

} // namespace tr::archive
