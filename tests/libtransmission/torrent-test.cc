// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef>
#include <ranges>

#include <libtransmission/torrent.h>

#include "test-fixtures.h"

using TorrentTest = tr::test::SessionTest;

namespace
{
auto constexpr TorFilenames = std::array{
    "Android-x86 8.1 r6 iso.torrent"sv,
    "debian-11.2.0-amd64-DVD-1.iso.torrent"sv,
    "ubuntu-18.04.6-desktop-amd64.iso.torrent"sv,
    "ubuntu-20.04.4-desktop-amd64.iso.torrent"sv,
};
}

TEST_F(TorrentTest, queueMoveUp)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 1, 3, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_up(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveDown)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 0, 2, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_down(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveTop)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 3, 1, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_top(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveBottom)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 2, 0, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_bottom(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, autoPrioritySortsBySize)
{
    session_->setAutoPriorityEnabled(true);
    session_->setAutoPriorityMaxHigh(1U);
    session_->setAutoPrioritySort(TR_AUTO_PRIORITY_SORT_SIZE);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);
    ASSERT_EQ(3U, tor->file_count());

    EXPECT_TRUE(tor->auto_priority());
    EXPECT_EQ(TR_PRI_HIGH, tr_torrentFile(tor, 2U).priority); // 512
    EXPECT_EQ(TR_PRI_NORMAL, tr_torrentFile(tor, 1U).priority); // 4096
    EXPECT_EQ(TR_PRI_LOW, tr_torrentFile(tor, 0U).priority); // 1048576
}

TEST_F(TorrentTest, autoPrioritySortsByName)
{
    session_->setAutoPriorityEnabled(true);
    session_->setAutoPriorityMaxHigh(1U);
    session_->setAutoPrioritySort(TR_AUTO_PRIORITY_SORT_NAME);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);
    ASSERT_EQ(3U, tor->file_count());

    tor->set_file_subpath(0U, "Zeta.bin");
    tor->set_file_subpath(1U, "alpha.bin");
    tor->set_file_subpath(2U, "beta.bin");
    tor->apply_auto_priorities();

    EXPECT_EQ(TR_PRI_LOW, tr_torrentFile(tor, 0U).priority);
    EXPECT_EQ(TR_PRI_HIGH, tr_torrentFile(tor, 1U).priority);
    EXPECT_EQ(TR_PRI_NORMAL, tr_torrentFile(tor, 2U).priority);
}

TEST_F(TorrentTest, autoPriorityKeepsUnwantedNormal)
{
    session_->setAutoPriorityEnabled(true);
    session_->setAutoPriorityMaxHigh(1U);
    session_->setAutoPrioritySort(TR_AUTO_PRIORITY_SORT_SIZE);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);
    ASSERT_EQ(3U, tor->file_count());

    auto const unwanted = tr_file_index_t{ 0 };
    tor->set_files_wanted(&unwanted, 1U, false);

    EXPECT_TRUE(tor->auto_priority());
    EXPECT_EQ(TR_PRI_NORMAL, tr_torrentFile(tor, unwanted).priority);
    EXPECT_EQ(TR_PRI_LOW, tr_torrentFile(tor, 1U).priority);
    EXPECT_EQ(TR_PRI_HIGH, tr_torrentFile(tor, 2U).priority);
}

TEST_F(TorrentTest, autoPriorityKeepsCompletedNormal)
{
    session_->setAutoPriorityEnabled(true);
    session_->setAutoPriorityMaxHigh(1U);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    ASSERT_EQ(3U, tor->file_count());

    for (tr_file_index_t i = 0; i < tor->file_count(); ++i)
    {
        EXPECT_EQ(TR_PRI_NORMAL, tr_torrentFile(tor, i).priority);
    }
}

TEST_F(TorrentTest, manualFilePriorityDisablesAutoPriority)
{
    session_->setAutoPriorityEnabled(true);
    session_->setAutoPriorityMaxHigh(1U);
    session_->setAutoPrioritySort(TR_AUTO_PRIORITY_SORT_SIZE);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);
    EXPECT_TRUE(tor->auto_priority());

    auto const file = tr_file_index_t{ 0U };
    tr_torrentSetFilePriorities(tor, &file, 1U, TR_PRI_HIGH);
    EXPECT_FALSE(tor->auto_priority());

    tor->set_auto_priority(true);
    tor->apply_auto_priorities();
    EXPECT_TRUE(tor->auto_priority());
    EXPECT_EQ(TR_PRI_HIGH, tr_torrentFile(tor, 2U).priority);
    EXPECT_EQ(TR_PRI_LOW, tr_torrentFile(tor, 0U).priority);
}
