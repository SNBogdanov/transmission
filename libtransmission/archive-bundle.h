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

namespace tr::archive
{

inline constexpr std::string_view BundleManifestFilename = "manifest.json";
inline constexpr uint64_t MaxBundleBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL;
inline constexpr size_t MaxBundleEntries = 65535U;

struct BundleEntryInfo
{
    std::string name;
    std::string comment;
    uint64_t size = 0;
    uint32_t crc = 0;
    uint64_t local_header_offset = 0;
};

struct BundleSource
{
    std::string name;
    std::string comment;
    std::string path;
    std::string inline_data;
};

[[nodiscard]] bool is_bundle_entry_name(std::string_view name);

[[nodiscard]] bool write_bundle(std::string_view path, std::vector<BundleSource> const& entries, tr_error* error = nullptr);

[[nodiscard]] bool read_bundle_index(std::string_view path, std::vector<BundleEntryInfo>& entries, tr_error* error = nullptr);

[[nodiscard]] bool extract_bundle_entry(
    std::string_view path,
    BundleEntryInfo const& entry,
    std::string_view destination,
    tr_error* error = nullptr);

[[nodiscard]] bool read_bundle_entry(
    std::string_view path,
    BundleEntryInfo const& entry,
    std::string& out,
    uint64_t max_size,
    tr_error* error = nullptr);

[[nodiscard]] BundleEntryInfo const* find_entry(std::vector<BundleEntryInfo> const& entries, std::string_view name);

} // namespace tr::archive
