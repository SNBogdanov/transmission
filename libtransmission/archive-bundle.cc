// This file Copyright Р’В© Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "libtransmission/archive-bundle.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/tr-strbuf.h"

using namespace std::literals;

namespace
{

constexpr uint32_t LocalHeaderSignature = 0x04034b50U;
constexpr uint32_t CentralDirectorySignature = 0x02014b50U;
constexpr uint32_t EndOfCentralDirectorySignature = 0x06054b50U;
constexpr uint16_t StoreMethod = 0U;
constexpr uint16_t Utf8Flag = 0x0800U;
constexpr size_t ChunkSize = 1024U * 1024U;
constexpr size_t MaxCentralDirectoryBytes = 64U * 1024U * 1024U;

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

[[nodiscard]] constexpr uint32_t make_crc32_table_entry(uint32_t value) noexcept
{
    for (size_t i = 0; i < 8U; ++i)
    {
        value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : value >> 1U;
    }

    return value;
}

[[nodiscard]] auto const& crc32_table() noexcept
{
    static auto const table = []
    {
        auto ret = std::array<uint32_t, 256>{};
        for (uint32_t i = 0; i < std::size(ret); ++i)
        {
            ret[i] = make_crc32_table_entry(i);
        }
        return ret;
    }();

    return table;
}

class Crc32
{
public:
    void update(std::string_view const data) noexcept
    {
        auto const& table = crc32_table();
        for (auto const ch : data)
        {
            crc_ = table[(crc_ ^ static_cast<uint8_t>(ch)) & 0xFFU] ^ (crc_ >> 8U);
        }
    }

    [[nodiscard]] uint32_t finish() const noexcept
    {
        return crc_ ^ 0xFFFFFFFFU;
    }

private:
    uint32_t crc_ = 0xFFFFFFFFU;
};

void append_u16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void append_u32(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

[[nodiscard]] uint16_t read_u16(std::string_view const data, size_t const offset) noexcept
{
    return static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) |
        static_cast<uint16_t>(static_cast<uint8_t>(data[offset + 1U]) << 8U);
}

[[nodiscard]] uint32_t read_u32(std::string_view const data, size_t const offset) noexcept
{
    return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1U])) << 8U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2U])) << 16U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3U])) << 24U);
}

[[nodiscard]] bool write_all(tr_sys_file_t const handle, std::string_view const data, uint64_t& offset, tr_error* error)
{
    auto remaining = data;
    while (!std::empty(remaining))
    {
        auto written = uint64_t{};
        if (!tr_sys_file_write(handle, std::data(remaining), std::size(remaining), &written, error))
        {
            return false;
        }

        if (written == 0U)
        {
            set_error(error, EIO, "couldn't write to bundle"sv);
            return false;
        }

        remaining.remove_prefix(static_cast<size_t>(written));
        offset += written;
    }

    return true;
}

[[nodiscard]] bool read_exact_at(
    tr_sys_file_t const handle,
    uint64_t const offset,
    void* const buffer,
    size_t const size,
    tr_error* error)
{
    auto done = size_t{};
    while (done < size)
    {
        auto n_read = uint64_t{};
        if (!tr_sys_file_read_at(handle, static_cast<char*>(buffer) + done, size - done, offset + done, &n_read, error))
        {
            return false;
        }

        if (n_read == 0U)
        {
            set_error(error, EIO, "unexpected end of bundle"sv);
            return false;
        }

        done += static_cast<size_t>(n_read);
    }

    return true;
}

[[nodiscard]] bool is_hex(char const ch) noexcept
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

[[nodiscard]] bool is_hash_string(std::string_view const sv) noexcept
{
    return std::size(sv) == 40U && std::ranges::all_of(sv, is_hex);
}

struct CentralEntry
{
    std::string name;
    std::string comment;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t local_header_offset = 0;
};

struct EndOfCentralDirectory
{
    uint16_t entry_count = 0;
    uint32_t central_directory_size = 0;
    uint32_t central_directory_offset = 0;
};

[[nodiscard]] bool find_end_of_central_directory(
    std::string_view const tail,
    uint64_t const file_size,
    EndOfCentralDirectory& out,
    tr_error* error)
{
    static constexpr size_t MinEocdSize = 22U;

    if (std::size(tail) < MinEocdSize)
    {
        set_error(error, EINVAL, "zip bundle is too short"sv);
        return false;
    }

    for (auto i = std::size(tail) - MinEocdSize;; --i)
    {
        if (read_u32(tail, i) == EndOfCentralDirectorySignature)
        {
            auto const comment_len = read_u16(tail, i + 20U);
            if (i + MinEocdSize + comment_len != std::size(tail))
            {
                continue;
            }

            out.entry_count = read_u16(tail, i + 10U);
            out.central_directory_size = read_u32(tail, i + 12U);
            out.central_directory_offset = read_u32(tail, i + 16U);

            if (out.entry_count == 0xFFFFU || out.central_directory_offset == 0xFFFFFFFFU)
            {
                set_error(error, EINVAL, "zip64 bundles are not supported"sv);
                return false;
            }

            if (out.central_directory_offset > file_size ||
                out.central_directory_size > file_size - out.central_directory_offset)
            {
                set_error(error, EINVAL, "zip bundle central directory is out of bounds"sv);
                return false;
            }

            return true;
        }

        if (i == 0U)
        {
            break;
        }
    }

    set_error(error, EINVAL, "zip bundle has no end-of-central-directory record"sv);
    return false;
}

[[nodiscard]] bool read_central_directory(
    std::string_view const data,
    EndOfCentralDirectory const& eocd,
    std::vector<CentralEntry>& entries,
    tr_error* error)
{
    if (eocd.entry_count > tr::archive::MaxBundleEntries)
    {
        set_error(error, EINVAL, "zip bundle has too many entries"sv);
        return false;
    }

    auto offset = size_t{};
    auto const end = std::size(data);
    entries.clear();
    entries.reserve(eocd.entry_count);
    auto names = std::set<std::string, std::less<>>{};

    for (uint16_t i = 0; i < eocd.entry_count; ++i)
    {
        static constexpr size_t FixedSize = 46U;

        if (offset > end || end - offset < FixedSize || read_u32(data, offset) != CentralDirectorySignature)
        {
            set_error(error, EINVAL, "zip bundle has a malformed central directory"sv);
            return false;
        }

        auto const flags = read_u16(data, offset + 8U);
        auto const method = read_u16(data, offset + 10U);
        auto const crc = read_u32(data, offset + 16U);
        auto const compressed_size = read_u32(data, offset + 20U);
        auto const uncompressed_size = read_u32(data, offset + 24U);
        auto const name_len = read_u16(data, offset + 28U);
        auto const extra_len = read_u16(data, offset + 30U);
        auto const comment_len = read_u16(data, offset + 32U);
        auto const local_header_offset = read_u32(data, offset + 42U);
        auto const variable_size = static_cast<size_t>(name_len) + extra_len + comment_len;

        if (offset + FixedSize + variable_size > end)
        {
            set_error(error, EINVAL, "zip bundle central directory entry is out of bounds"sv);
            return false;
        }

        if ((flags & 0x0001U) != 0U)
        {
            set_error(error, EINVAL, "encrypted zip entries are not supported"sv);
            return false;
        }

        if ((flags & 0x0008U) != 0U)
        {
            set_error(error, EINVAL, "zip data descriptors are not supported"sv);
            return false;
        }

        if (method != StoreMethod || compressed_size != uncompressed_size)
        {
            set_error(error, EINVAL, "only stored zip entries are supported"sv);
            return false;
        }

        auto name = std::string{ data.substr(offset + FixedSize, name_len) };
        if (!tr::archive::is_bundle_entry_name(name))
        {
            set_error(error, EINVAL, "zip bundle contains an unexpected entry name"sv);
            return false;
        }

        if (!names.emplace(name).second)
        {
            set_error(error, EINVAL, "zip bundle contains duplicate entries"sv);
            return false;
        }

        entries.push_back(
            CentralEntry{
                std::move(name),
                std::string{ data.substr(offset + FixedSize + name_len + extra_len, comment_len) },
                crc,
                compressed_size,
                uncompressed_size,
                local_header_offset,
            });

        offset += FixedSize + variable_size;
    }

    if (offset != end)
    {
        set_error(error, EINVAL, "zip bundle central directory size does not match its entries"sv);
        return false;
    }

    return true;
}

[[nodiscard]] bool read_payload(
    tr_sys_file_t const handle,
    CentralEntry const& entry,
    uint64_t const file_size,
    std::string* const out,
    tr_sys_file_t const out_handle,
    uint64_t const out_limit,
    tr_error* error)
{
    static constexpr size_t LocalHeaderFixedSize = 30U;

    if (entry.local_header_offset > file_size || file_size - entry.local_header_offset < LocalHeaderFixedSize)
    {
        set_error(error, EINVAL, "zip bundle has a malformed local file header"sv);
        return false;
    }

    auto local_header = std::array<char, LocalHeaderFixedSize>{};
    if (!read_exact_at(handle, entry.local_header_offset, std::data(local_header), std::size(local_header), error))
    {
        return false;
    }

    auto const local = std::string_view{ std::data(local_header), std::size(local_header) };
    if (read_u32(local, 0U) != LocalHeaderSignature)
    {
        set_error(error, EINVAL, "zip bundle has a malformed local file header"sv);
        return false;
    }

    auto const flags = read_u16(local, 6U);
    auto const method = read_u16(local, 8U);
    auto const local_crc = read_u32(local, 14U);
    auto const local_compressed_size = read_u32(local, 18U);
    auto const local_uncompressed_size = read_u32(local, 22U);
    auto const name_len = read_u16(local, 26U);
    auto const extra_len = read_u16(local, 28U);

    if ((flags & 0x0001U) != 0U || (flags & 0x0008U) != 0U || method != StoreMethod)
    {
        set_error(error, EINVAL, "zip bundle entry uses unsupported flags or compression"sv);
        return false;
    }

    if (local_crc != entry.crc || local_compressed_size != entry.compressed_size ||
        local_uncompressed_size != entry.uncompressed_size)
    {
        set_error(error, EINVAL, "zip bundle local header does not match the central directory"sv);
        return false;
    }

    auto const local_name_offset = entry.local_header_offset + LocalHeaderFixedSize;
    if (local_name_offset > file_size || name_len > file_size - local_name_offset)
    {
        set_error(error, EINVAL, "zip bundle local file name is out of bounds"sv);
        return false;
    }

    auto local_name = std::string(name_len, '\0');
    if (name_len > 0U && !read_exact_at(handle, local_name_offset, std::data(local_name), name_len, error))
    {
        return false;
    }

    if (local_name != entry.name)
    {
        set_error(error, EINVAL, "zip bundle local file name does not match the central directory"sv);
        return false;
    }

    auto const data_offset = entry.local_header_offset + LocalHeaderFixedSize + name_len + extra_len;
    if (data_offset > file_size || entry.compressed_size > file_size - data_offset)
    {
        set_error(error, EINVAL, "zip bundle entry data is out of bounds"sv);
        return false;
    }

    if (out_limit != 0U && entry.uncompressed_size > out_limit)
    {
        set_error(error, EINVAL, "zip bundle entry is too large"sv);
        return false;
    }

    auto buffer = std::vector<char>(ChunkSize);
    auto crc = Crc32{};
    auto remaining = uint64_t{ entry.compressed_size };
    auto offset = data_offset;

    while (remaining > 0U)
    {
        auto const n = static_cast<size_t>(std::min<uint64_t>(remaining, std::size(buffer)));
        if (!read_exact_at(handle, offset, std::data(buffer), n, error))
        {
            return false;
        }

        auto const chunk = std::string_view{ std::data(buffer), n };
        crc.update(chunk);

        if (out != nullptr)
        {
            out->append(chunk);
        }
        else if (out_handle != TR_BAD_SYS_FILE)
        {
            auto written = uint64_t{};
            if (!tr_sys_file_write(out_handle, std::data(buffer), n, &written, error) || written != n)
            {
                if (!has_error(error))
                {
                    set_error(error, EIO, "couldn't write bundle entry"sv);
                }
                return false;
            }
        }

        offset += n;
        remaining -= n;
    }

    if (crc.finish() != entry.crc)
    {
        set_error(error, EINVAL, "zip bundle entry failed its CRC32 check"sv);
        return false;
    }

    return true;
}

} // namespace

namespace tr::archive
{

bool is_bundle_entry_name(std::string_view const name)
{
    if (name == BundleManifestFilename)
    {
        return true;
    }

    auto const dot = name.rfind('.');
    if (dot == std::string_view::npos || dot != 40U)
    {
        return false;
    }

    static constexpr auto Suffixes = std::array<std::string_view, 4>{
        ".torrent"sv,
        ".resume"sv,
        ".json"sv,
        ".magnet"sv,
    };

    return is_hash_string(name.substr(0U, dot)) && std::ranges::find(Suffixes, name.substr(dot)) != std::ranges::end(Suffixes);
}

bool write_bundle(std::string_view const path, std::vector<BundleSource> const& entries, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    if (std::empty(entries))
    {
        set_error(error, EINVAL, "bundle has no entries"sv);
        return false;
    }

    if (std::size(entries) >= 0xFFFFU)
    {
        set_error(error, EINVAL, "too many bundle entries"sv);
        return false;
    }

    auto const parent = tr_sys_path_dirname(path);
    if (!std::empty(parent))
    {
        tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777);
    }

    auto tmp_path = tr_pathbuf{ path, ".tmp.XXXXXX"sv };
    auto const handle = tr_sys_file_open_temp(std::data(tmp_path), error);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto ok = true;
    auto central_directory = std::string{};
    auto central_entries = size_t{};
    auto offset = uint64_t{};
    auto buffer = std::vector<char>(ChunkSize);

    for (auto const& entry : entries)
    {
        if (!is_bundle_entry_name(entry.name))
        {
            set_error(error, EINVAL, "invalid bundle entry name"sv);
            ok = false;
            break;
        }

        if (std::size(entry.name) > UINT16_MAX || std::size(entry.comment) > UINT16_MAX)
        {
            set_error(error, EINVAL, "bundle entry name or comment is too long"sv);
            ok = false;
            break;
        }

        auto const* inline_data = entry.path.empty() ? &entry.inline_data : nullptr;
        auto file_handle = TR_BAD_SYS_FILE;
        auto file_size = uint64_t{ 0 };
        auto const close_source = [&file_handle]()
        {
            if (file_handle != TR_BAD_SYS_FILE)
            {
                tr_sys_file_close(file_handle);
                file_handle = TR_BAD_SYS_FILE;
            }
        };

        if (inline_data == nullptr)
        {
            auto const info = tr_sys_path_get_info(entry.path, 0, error);
            if (!info || !info->isFile())
            {
                if (!has_error(error))
                {
                    set_error(error, ENOENT, "couldn't stat bundle source file"sv);
                }
                ok = false;
                break;
            }

            file_size = info->size;
            file_handle = tr_sys_file_open(entry.path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
            if (file_handle == TR_BAD_SYS_FILE)
            {
                ok = false;
                break;
            }
        }
        else
        {
            file_size = std::size(*inline_data);
        }

        if (file_size > UINT32_MAX)
        {
            set_error(error, EINVAL, "bundle entry is too large for a non-zip64 file"sv);
            close_source();
            ok = false;
            break;
        }

        auto crc = Crc32{};
        if (inline_data != nullptr)
        {
            crc.update(*inline_data);
        }
        else
        {
            auto remaining = file_size;
            auto file_offset = uint64_t{};
            while (remaining > 0U)
            {
                auto const n = static_cast<size_t>(std::min<uint64_t>(remaining, std::size(buffer)));
                if (!read_exact_at(file_handle, file_offset, std::data(buffer), n, error))
                {
                    ok = false;
                    break;
                }

                crc.update(std::string_view{ std::data(buffer), n });
                file_offset += n;
                remaining -= n;
            }

            if (!ok)
            {
                close_source();
                break;
            }
        }

        if (offset > UINT32_MAX)
        {
            set_error(error, EINVAL, "bundle is too large for a non-zip64 file"sv);
            close_source();
            ok = false;
            break;
        }

        auto const local_header_offset = static_cast<uint32_t>(offset);
        auto const crc_value = crc.finish();
        auto const size = static_cast<uint32_t>(file_size);
        auto const name_len = static_cast<uint16_t>(std::size(entry.name));
        auto const comment_len = static_cast<uint16_t>(std::size(entry.comment));

        auto header = std::string{};
        append_u32(header, LocalHeaderSignature);
        append_u16(header, 20U); // version needed to extract
        append_u16(header, Utf8Flag);
        append_u16(header, StoreMethod);
        append_u16(header, 0U); // last mod time
        append_u16(header, 0x21U); // last mod date: 1980-01-01
        append_u32(header, crc_value);
        append_u32(header, size);
        append_u32(header, size);
        append_u16(header, name_len);
        append_u16(header, 0U); // extra field length
        header.append(entry.name);

        if (!write_all(handle, header, offset, error))
        {
            close_source();
            ok = false;
            break;
        }

        if (inline_data != nullptr)
        {
            if (!write_all(handle, *inline_data, offset, error))
            {
                ok = false;
                break;
            }
        }
        else
        {
            auto remaining = file_size;
            auto file_offset = uint64_t{};
            while (remaining > 0U)
            {
                auto const n = static_cast<size_t>(std::min<uint64_t>(remaining, std::size(buffer)));
                if (!read_exact_at(file_handle, file_offset, std::data(buffer), n, error))
                {
                    ok = false;
                    break;
                }

                if (!write_all(handle, std::string_view{ std::data(buffer), n }, offset, error))
                {
                    ok = false;
                    break;
                }

                file_offset += n;
                remaining -= n;
            }

            close_source();

            if (!ok)
            {
                break;
            }
        }

        if (std::size(central_directory) > MaxCentralDirectoryBytes)
        {
            set_error(error, EINVAL, "bundle central directory is too large"sv);
            close_source();
            ok = false;
            break;
        }

        if (central_entries == 0xFFFFU)
        {
            set_error(error, EINVAL, "too many entries for a non-zip64 bundle"sv);
            close_source();
            ok = false;
            break;
        }

        append_u32(central_directory, CentralDirectorySignature);
        append_u16(central_directory, 20U); // version made by
        append_u16(central_directory, 20U); // version needed
        append_u16(central_directory, Utf8Flag);
        append_u16(central_directory, StoreMethod);
        append_u16(central_directory, 0U); // last mod time
        append_u16(central_directory, 0x21U); // last mod date
        append_u32(central_directory, crc_value);
        append_u32(central_directory, size);
        append_u32(central_directory, size);
        append_u16(central_directory, name_len);
        append_u16(central_directory, 0U); // extra field length
        append_u16(central_directory, comment_len);
        append_u16(central_directory, 0U); // disk number start
        append_u16(central_directory, 0U); // internal attributes
        append_u32(central_directory, 0U); // external attributes
        append_u32(central_directory, local_header_offset);
        central_directory.append(entry.name);
        central_directory.append(entry.comment);

        ++central_entries;
    }

    if (ok)
    {
        if (offset > UINT32_MAX)
        {
            set_error(error, EINVAL, "bundle is too large for a non-zip64 file"sv);
            ok = false;
        }
        else if (std::size(central_directory) > MaxBundleBytes - offset)
        {
            set_error(error, EINVAL, "bundle is too large for a non-zip64 file"sv);
            ok = false;
        }
        else
        {
            auto const central_directory_offset = static_cast<uint32_t>(offset);
            if (!write_all(handle, central_directory, offset, error))
            {
                ok = false;
            }
            else
            {
                auto footer = std::string{};
                append_u32(footer, EndOfCentralDirectorySignature);
                append_u16(footer, 0U); // number of this disk
                append_u16(footer, 0U); // disk with central directory
                append_u16(footer, static_cast<uint16_t>(central_entries));
                append_u16(footer, static_cast<uint16_t>(central_entries));
                append_u32(footer, static_cast<uint32_t>(std::size(central_directory)));
                append_u32(footer, central_directory_offset);
                append_u16(footer, 0U); // comment length
                ok = write_all(handle, footer, offset, error);
            }
        }
    }

    if (tr_sys_file_close(handle, error) && ok)
    {
        if (tr_sys_path_rename(tmp_path, path, error))
        {
            return true;
        }
    }

    tr_sys_path_remove(tmp_path);
    return false;
}

bool read_bundle_index(std::string_view const path, std::vector<BundleEntryInfo>& entries, tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    entries.clear();

    auto const info = tr_sys_path_get_info(path, 0, error);
    if (!info || !info->isFile())
    {
        if (!has_error(error))
        {
            set_error(error, ENOENT, "couldn't stat bundle"sv);
        }
        return false;
    }

    if (info->size > MaxBundleBytes)
    {
        set_error(error, EINVAL, "bundle is too large"sv);
        return false;
    }

    auto const handle = tr_sys_file_open(path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto ok = false;
    do
    {
        static constexpr uint64_t MaxEocdSearch = 22U + 0xFFFFU;
        auto const tail_size = static_cast<size_t>(std::min<uint64_t>(info->size, MaxEocdSearch));
        auto tail = std::vector<char>(tail_size);
        if (!read_exact_at(handle, info->size - tail_size, std::data(tail), tail_size, error))
        {
            break;
        }

        auto eocd = EndOfCentralDirectory{};
        if (!find_end_of_central_directory(std::string_view{ std::data(tail), std::size(tail) }, info->size, eocd, error))
        {
            break;
        }

        if (eocd.entry_count == 0U || eocd.central_directory_size == 0U)
        {
            set_error(error, EINVAL, "bundle has no entries"sv);
            break;
        }

        if (eocd.central_directory_size > MaxCentralDirectoryBytes)
        {
            set_error(error, EINVAL, "bundle central directory is too large"sv);
            break;
        }

        auto central = std::vector<char>(eocd.central_directory_size);
        if (!read_exact_at(handle, eocd.central_directory_offset, std::data(central), std::size(central), error))
        {
            break;
        }

        auto central_entries = std::vector<CentralEntry>{};
        if (!read_central_directory(std::string_view{ std::data(central), std::size(central) }, eocd, central_entries, error))
        {
            break;
        }

        entries.reserve(std::size(central_entries));
        for (auto& entry : central_entries)
        {
            entries.push_back(
                BundleEntryInfo{
                    std::move(entry.name),
                    std::move(entry.comment),
                    entry.uncompressed_size,
                    entry.crc,
                    entry.local_header_offset,
                });
        }

        ok = true;
    } while (false);

    tr_sys_file_close(handle);
    return ok;
}

bool extract_bundle_entry(
    std::string_view const path,
    BundleEntryInfo const& entry,
    std::string_view const destination,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    auto const handle = tr_sys_file_open(path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto const parent = tr_sys_path_dirname(destination);
    if (!std::empty(parent))
    {
        tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777);
    }

    auto tmp_path = tr_pathbuf{ destination, ".tmp.XXXXXX"sv };
    auto const out_handle = tr_sys_file_open_temp(std::data(tmp_path), error);
    if (out_handle == TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(handle);
        return false;
    }

    auto const info = tr_sys_path_get_info(path, 0, error);
    auto const central = CentralEntry{
        entry.name,
        entry.comment,
        entry.crc,
        static_cast<uint32_t>(entry.size),
        static_cast<uint32_t>(entry.size),
        static_cast<uint32_t>(entry.local_header_offset),
    };

    auto ok = info && read_payload(handle, central, info->size, nullptr, out_handle, 0U, error);
    ok = tr_sys_file_close(out_handle, error) && ok;
    tr_sys_file_close(handle);

    if (ok && tr_sys_path_rename(tmp_path, destination, error))
    {
        return true;
    }

    tr_sys_path_remove(tmp_path);
    return false;
}

bool read_bundle_entry(
    std::string_view const path,
    BundleEntryInfo const& entry,
    std::string& out,
    uint64_t const max_size,
    tr_error* error)
{
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    out.clear();

    auto const handle = tr_sys_file_open(path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (handle == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto const info = tr_sys_path_get_info(path, 0, error);
    auto const central = CentralEntry{
        entry.name,
        entry.comment,
        entry.crc,
        static_cast<uint32_t>(entry.size),
        static_cast<uint32_t>(entry.size),
        static_cast<uint32_t>(entry.local_header_offset),
    };

    auto const ok = info && read_payload(handle, central, info->size, &out, TR_BAD_SYS_FILE, max_size, error);
    tr_sys_file_close(handle);

    if (ok)
    {
        return true;
    }

    out.clear();
    return false;
}

BundleEntryInfo const* find_entry(std::vector<BundleEntryInfo> const& entries, std::string_view const name)
{
    auto const iter = std::ranges::find_if(entries, [name](auto const& entry) { return entry.name == name; });
    return iter == std::ranges::end(entries) ? nullptr : &*iter;
}

} // namespace tr::archive
