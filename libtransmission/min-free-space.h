// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct tr_min_free_space_rule
{
    std::string path;
    uint64_t bytes = 0;
};

// Parses a byte count with an optional binary suffix:
// e.g. "1024", "512K", "10M", "50G", "2T", "64MiB".
[[nodiscard]] std::optional<uint64_t> tr_parse_byte_size(std::string_view text);

// Parses "PATH=SIZE" entries into normalized rules.
[[nodiscard]] std::vector<tr_min_free_space_rule> tr_min_free_space_parse(std::vector<std::string> const& specs);

// Returns the limit for `dir`: the nearest ancestor (or `dir` itself) that has
// a rule. Returns nullopt when no rule applies.
[[nodiscard]] std::optional<uint64_t> tr_min_free_space_for(
    std::vector<tr_min_free_space_rule> const& rules,
    std::string_view dir);
