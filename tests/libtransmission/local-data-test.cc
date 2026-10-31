// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/error.h>
#include <libtransmission/local-data.h>

using namespace std::literals;

namespace
{

// LocalData calls back on its own worker threads, so callers can't assume
// the callback has run by the time the call returns. Poll a flag instead.
template<typename Predicate>
[[nodiscard]] bool wait_for(Predicate predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }

        std::this_thread::sleep_for(1ms);
    }

    return predicate();
}

class StubBackend final : public tr::LocalData::Backend
{
public:
    [[nodiscard]] tr_error_code_t read(
        [[maybe_unused]] tr_torrent_id_t tor_id,
        tr_byte_span_t byte_span,
        tr::LocalData::BlockData& setme) override
    {
        read_span = byte_span;
        setme.assign({ uint8_t{ 1U }, uint8_t{ 2U }, uint8_t{ 3U } });
        return read_err;
    }

    [[nodiscard]] tr_error_code_t test_piece(
        [[maybe_unused]] tr_torrent_id_t tor_id,
        tr_piece_index_t piece,
        tr_sha1_digest_t& setme_hash) override
    {
        tested_piece = piece;
        setme_hash = hash;
        return test_err;
    }

    [[nodiscard]] tr_error_code_t write(
        [[maybe_unused]] tr_torrent_id_t tor_id,
        tr_byte_span_t byte_span,
        tr::LocalData::BlockData const& data) override
    {
        while (write_block && !allow_write)
        {
            std::this_thread::sleep_for(1ms);
        }

        write_span = byte_span;
        last_write.assign(std::begin(data), std::end(data));
        return write_err;
    }

    [[nodiscard]] tr_error_code_t move(
        [[maybe_unused]] tr_torrent_id_t id,
        std::string_view old_parent,
        std::string_view parent,
        std::string_view parent_name) override
    {
        moved_from = std::string{ old_parent };
        moved_to = std::string{ parent };
        moved_name = std::string{ parent_name };
        return move_err;
    }

    [[nodiscard]] tr_error_code_t remove(
        [[maybe_unused]] tr_torrent_id_t id,
        [[maybe_unused]] tr_torrent_remove_func remove_func) override
    {
        remove_called = true;
        return remove_err;
    }

    void rename(tr_torrent_id_t id, std::string_view oldpath, std::string_view newname, tr_torrent_rename_done_func callback)
        override
    {
        renamed_from = std::string{ oldpath };
        renamed_to = std::string{ newname };
        if (callback != nullptr)
        {
            callback(id, oldpath, newname, tr_error{});
        }
    }

    void close_all() override
    {
        close_all_called = true;
    }

    void close_torrent(tr_torrent_id_t tor_id) override
    {
        closed_torrent = tor_id;
    }

    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num) override
    {
        auto const lock = std::lock_guard{ closed_file_mutex };
        closed_file = std::pair{ tor_id, file_num };
    }

    [[nodiscard]] std::optional<std::pair<tr_torrent_id_t, tr_file_index_t>> get_closed_file() const
    {
        auto const lock = std::lock_guard{ closed_file_mutex };
        return closed_file;
    }

    tr_error_code_t read_err = 0;
    tr_error_code_t test_err = 0;
    tr_error_code_t write_err = 0;
    tr_error_code_t move_err = 0;
    tr_error_code_t remove_err = 0;
    std::atomic<bool> write_block = false;
    std::atomic<bool> allow_write = true;
    std::atomic<bool> remove_called = false;
    std::atomic<bool> close_all_called = false;
    tr_byte_span_t read_span{};
    tr_byte_span_t write_span{};
    tr_piece_index_t tested_piece = 0;
    tr_sha1_digest_t hash = tr_sha1::digest("local-data-test"sv);
    std::vector<uint8_t> last_write;
    std::string moved_from;
    std::string moved_to;
    std::string moved_name;
    std::string renamed_from;
    std::string renamed_to;
    std::atomic<tr_torrent_id_t> closed_torrent = -1;
    mutable std::mutex closed_file_mutex;
    std::optional<std::pair<tr_torrent_id_t, tr_file_index_t>> closed_file;
};

} // namespace

TEST(LocalData, Read)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend) };

    auto done = std::atomic<bool>{ false };
    local_data.read(
        7,
        { .begin = 10U, .end = 13U },
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_byte_span_t byte_span, tr_error const& error, auto data)
        {
            EXPECT_EQ(7, tor_id);
            EXPECT_EQ(raw_backend->read_span.begin, byte_span.begin);
            EXPECT_EQ(raw_backend->read_span.end, byte_span.end);
            EXPECT_FALSE(error);
            ASSERT_NE(nullptr, data);
            EXPECT_EQ((std::vector<uint8_t>{ 1U, 2U, 3U }), std::vector<uint8_t>(std::begin(*data), std::end(*data)));
            done = true;
        });

    EXPECT_TRUE(wait_for([&done]() { return done.load(); }));
}

TEST(LocalData, TestPiece)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend) };

    auto done = std::atomic<bool>{ false };
    local_data.test_piece(
        9,
        3,
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_piece_index_t piece, tr_error const& error, auto hash)
        {
            EXPECT_EQ(9, tor_id);
            EXPECT_EQ(raw_backend->tested_piece, piece);
            EXPECT_FALSE(error);
            ASSERT_TRUE(hash.has_value());
            EXPECT_EQ(raw_backend->hash, *hash);
            done = true;
        });

    EXPECT_TRUE(wait_for([&done]() { return done.load(); }));
}

TEST(LocalData, Write)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend) };

    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 4U }, uint8_t{ 5U }, uint8_t{ 6U } });

    auto done = std::atomic<bool>{ false };
    local_data.write(
        11,
        { .begin = 20U, .end = 23U },
        std::move(data),
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_byte_span_t byte_span, tr_error const& error)
        {
            EXPECT_EQ(11, tor_id);
            EXPECT_EQ(raw_backend->write_span.begin, byte_span.begin);
            EXPECT_EQ(raw_backend->write_span.end, byte_span.end);
            EXPECT_FALSE(error);
            EXPECT_EQ((std::vector<uint8_t>{ 4U, 5U, 6U }), raw_backend->last_write);
            done = true;
        });

    EXPECT_TRUE(wait_for([&done]() { return done.load(); }));
    EXPECT_EQ(0U, local_data.enqueued_write_bytes());
}

TEST(LocalData, WriteQueueLimitPerTorrent)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    raw_backend->write_block = true;
    raw_backend->allow_write = false;

    auto local_data = tr::LocalData{ std::move(backend), 1 };

    auto constexpr BlockSize = size_t{ TrBlockSize };
    auto const n_blocks = tr::LocalData::MaxPendingWriteBytesPerTorrent / BlockSize;
    auto write_count = std::atomic<size_t>{};

    for (size_t i = 0; i < n_blocks; ++i)
    {
        auto data = std::make_unique<tr::LocalData::BlockData>();
        data->resize(BlockSize);
        local_data.write(
            1,
            { .begin = i * BlockSize, .end = (i + 1) * BlockSize },
            std::move(data),
            [&write_count](tr_torrent_id_t, tr_byte_span_t, tr_error const& error)
            {
                EXPECT_FALSE(error);
                ++write_count;
            });
    }

    auto rejected = std::atomic<bool>{ false };
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->resize(BlockSize);
    local_data.write(
        1,
        { .begin = n_blocks * BlockSize, .end = (n_blocks + 1) * BlockSize },
        std::move(data),
        [&rejected](tr_torrent_id_t, tr_byte_span_t, tr_error const& error)
        {
            EXPECT_EQ(EAGAIN, error.code());
            rejected = true;
        });

    EXPECT_TRUE(rejected.load());

    raw_backend->allow_write = true;
    EXPECT_TRUE(wait_for([&write_count, n_blocks]() { return write_count.load() == n_blocks; }));
    EXPECT_EQ(0U, local_data.pending_write_bytes(1));
    EXPECT_EQ(0U, local_data.enqueued_write_bytes());
}

TEST(LocalData, AdminOperationsDelegate)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend) };

    auto move_called = std::atomic<bool>{ false };
    local_data.move(
        5,
        "/old",
        "/new",
        "name",
        [&move_called](tr_torrent_id_t tor_id, tr_error const& error)
        {
            EXPECT_EQ(5, tor_id);
            EXPECT_FALSE(error);
            move_called = true;
        });
    EXPECT_TRUE(wait_for([&move_called]() { return move_called.load(); }));
    EXPECT_EQ("/old", raw_backend->moved_from);
    EXPECT_EQ("/new", raw_backend->moved_to);
    EXPECT_EQ("name", raw_backend->moved_name);

    auto rename_called = std::atomic<bool>{ false };
    local_data.rename(
        8,
        "old",
        "new",
        [&rename_called](tr_torrent_id_t tor_id, std::string_view oldpath, std::string_view newname, tr_error const& error)
        {
            EXPECT_EQ(8, tor_id);
            EXPECT_EQ("old", oldpath);
            EXPECT_EQ("new", newname);
            EXPECT_FALSE(error);
            rename_called = true;
        });
    EXPECT_TRUE(wait_for([&rename_called]() { return rename_called.load(); }));

    local_data.remove(12, {});
    EXPECT_TRUE(wait_for([raw_backend]() { return raw_backend->remove_called.load(); }));

    local_data.close_file(13, 2);
    EXPECT_TRUE(wait_for(
        [raw_backend]()
        {
            auto const opt = raw_backend->get_closed_file();
            return opt.has_value() && opt->first == 13 && opt->second == 2;
        }));

    local_data.close_torrent(14);
    EXPECT_TRUE(wait_for([raw_backend]() { return raw_backend->closed_torrent.load() == 14; }));

    local_data.close_all();
    EXPECT_TRUE(raw_backend->close_all_called.load());

    local_data.shutdown();
}
