// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/archive-bundle.h>
#include <libtransmission/archive.h>
#include <libtransmission/error.h>
#include <libtransmission/file-utils.h>
#include <libtransmission/file.h>

#include "test-fixtures.h"

using namespace std::literals;
using tr::test::SandboxedTest;

namespace
{

class ArchiveBundleTest : public SandboxedTest
{
protected:
    [[nodiscard]] std::string bundlePath() const
    {
        return sandboxDir() + "/bundle.zip";
    }
};

} // namespace

TEST_F(ArchiveBundleTest, archiveNames)
{
    EXPECT_TRUE(tr::archive::is_valid_name("backup"sv));
    EXPECT_TRUE(tr::archive::is_valid_name("backup.2026"sv));

    EXPECT_FALSE(tr::archive::is_valid_name(""sv));
    EXPECT_FALSE(tr::archive::is_valid_name("."sv));
    EXPECT_FALSE(tr::archive::is_valid_name(".."sv));
    EXPECT_FALSE(tr::archive::is_valid_name("name."sv));
    EXPECT_FALSE(tr::archive::is_valid_name("name "sv));
    EXPECT_FALSE(tr::archive::is_valid_name("exports"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("all"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("CON"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("nul.txt"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("Lpt9.tar"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("bad/name"sv));
    EXPECT_FALSE(tr::archive::is_valid_name("bad\\name"sv));
}

TEST_F(ArchiveBundleTest, entryNames)
{
    EXPECT_TRUE(tr::archive::is_bundle_entry_name("manifest.json"sv));
    EXPECT_TRUE(tr::archive::is_bundle_entry_name("0123456789abcdef0123456789abcdef01234567.torrent"sv));
    EXPECT_TRUE(tr::archive::is_bundle_entry_name("0123456789ABCDEF0123456789ABCDEF01234567.resume"sv));

    EXPECT_FALSE(tr::archive::is_bundle_entry_name(""sv));
    EXPECT_FALSE(tr::archive::is_bundle_entry_name("../manifest.json"sv));
    EXPECT_FALSE(tr::archive::is_bundle_entry_name("0123456789abcdef0123456789abcdef0123456.torrent"sv));
    EXPECT_FALSE(tr::archive::is_bundle_entry_name("0123456789abcdef0123456789abcdef01234567.exe"sv));
}

TEST_F(ArchiveBundleTest, rejectsInvalidEntryNames)
{
    auto const entries = std::vector<tr::archive::BundleSource>{
        { "../manifest.json", {}, {}, R"({"version":1})" },
    };

    auto error = tr_error{};
    EXPECT_FALSE(tr::archive::write_bundle(bundlePath(), entries, &error));
    EXPECT_TRUE(error);
    EXPECT_FALSE(tr_sys_path_exists(bundlePath()));
}

TEST_F(ArchiveBundleTest, roundTrip)
{
    auto const path = bundlePath();
    auto const payload = std::string{ "hello from a stored zip entry"sv };
    auto const comment = "Transmission archive torrent metainfo; hash=0123456789abcdef0123456789abcdef01234567"sv;
    auto const entries = std::vector<tr::archive::BundleSource>{
        { "manifest.json", std::string{ "Transmission archive manifest"sv }, {}, R"({"version":1})" },
        { "0123456789abcdef0123456789abcdef01234567.torrent", std::string{ comment }, {}, payload },
        { "0123456789abcdef0123456789abcdef01234567.resume", std::string{ "resume data"sv }, {}, "resume"sv },
    };

    auto error = tr_error{};
    ASSERT_TRUE(tr::archive::write_bundle(path, entries, &error)) << error;

    auto index = std::vector<tr::archive::BundleEntryInfo>{};
    ASSERT_TRUE(tr::archive::read_bundle_index(path, index, &error)) << error;
    ASSERT_EQ(std::size(entries), std::size(index));

    auto const* const torrent_entry = tr::archive::find_entry(index, "0123456789abcdef0123456789abcdef01234567.torrent"sv);
    ASSERT_NE(torrent_entry, nullptr);
    EXPECT_EQ(comment, std::string_view{ torrent_entry->comment });
    EXPECT_EQ(std::size(payload), torrent_entry->size);

    auto contents = std::string{};
    ASSERT_TRUE(tr::archive::read_bundle_entry(path, *torrent_entry, contents, 1024U, &error)) << error;
    EXPECT_EQ(payload, contents);

    auto const extracted_path = sandboxDir() + "/extracted.torrent";
    ASSERT_TRUE(tr::archive::extract_bundle_entry(path, *torrent_entry, extracted_path, &error)) << error;
    auto extracted = std::vector<char>{};
    ASSERT_TRUE(tr_file_read(extracted_path, extracted, &error)) << error;
    EXPECT_EQ(payload, std::string{ std::data(extracted), std::size(extracted) });
}

TEST_F(ArchiveBundleTest, rejectsOversizedEntry)
{
    auto const path = bundlePath();
    auto const payload = std::string(2048U, 'x');
    auto const entries = std::vector<tr::archive::BundleSource>{
        { "manifest.json", {}, {}, R"({"version":1})" },
        { "0123456789abcdef0123456789abcdef01234567.torrent", {}, {}, payload },
    };

    auto error = tr_error{};
    ASSERT_TRUE(tr::archive::write_bundle(path, entries, &error)) << error;

    auto index = std::vector<tr::archive::BundleEntryInfo>{};
    ASSERT_TRUE(tr::archive::read_bundle_index(path, index, &error)) << error;

    auto const* const torrent_entry = tr::archive::find_entry(index, "0123456789abcdef0123456789abcdef01234567.torrent"sv);
    ASSERT_NE(torrent_entry, nullptr);

    auto contents = std::string{};
    EXPECT_FALSE(tr::archive::read_bundle_entry(path, *torrent_entry, contents, 1024U, &error));
    EXPECT_TRUE(contents.empty());
    EXPECT_TRUE(error);
}

TEST_F(ArchiveBundleTest, detectsCorruption)
{
    auto const path = bundlePath();
    auto const payload = std::string{ "corruption detection payload"sv };
    auto const entries = std::vector<tr::archive::BundleSource>{
        { "manifest.json", {}, {}, R"({"version":1})" },
        { "0123456789abcdef0123456789abcdef01234567.torrent", {}, {}, payload },
    };

    auto error = tr_error{};
    ASSERT_TRUE(tr::archive::write_bundle(path, entries, &error)) << error;

    auto data = std::vector<char>{};
    ASSERT_TRUE(tr_file_read(path, data, &error)) << error;
    auto const contents = std::string_view{ std::data(data), std::size(data) };
    auto const pos = contents.find(payload);
    ASSERT_NE(pos, std::string_view::npos);
    data[pos] = 'C';
    ASSERT_TRUE(tr_file_save(path, std::string_view{ std::data(data), std::size(data) }, &error)) << error;

    auto index = std::vector<tr::archive::BundleEntryInfo>{};
    ASSERT_TRUE(tr::archive::read_bundle_index(path, index, &error)) << error;

    auto const* const torrent_entry = tr::archive::find_entry(index, "0123456789abcdef0123456789abcdef01234567.torrent"sv);
    ASSERT_NE(torrent_entry, nullptr);

    auto out = std::string{};
    EXPECT_FALSE(tr::archive::read_bundle_entry(path, *torrent_entry, out, 1024U, &error));
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(error);
}
