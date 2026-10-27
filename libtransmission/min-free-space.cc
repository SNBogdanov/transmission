// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/min-free-space.h"
#include "libtransmission/string-utils.h"

namespace
{
[[nodiscard]] std::string normalize_path(std::string_view path)
{
    auto resolved = tr_sys_path_resolve(path);
    auto result = resolved.empty() ? std::string{ path } : std::move(resolved);

    std::replace(std::begin(result), std::end(result), '\\', '/');

    while (std::size(result) > 1 && result.back() == '/')
    {
        result.pop_back();
    }

#ifdef _WIN32
    result = tr_strlower(result);
#endif

    return result;
}

[[nodiscard]] bool is_ancestor_or_equal(std::string_view ancestor, std::string_view path)
{
    if (ancestor == path)
    {
        return true;
    }

    if (ancestor == "/")
    {
        return !std::empty(path) && path.front() == '/';
    }

#ifdef _WIN32
    if (std::size(ancestor) == 3 && ancestor[1] == ':' && ancestor[2] == '/')
    {
        return std::size(path) > 3 && tr_strv_starts_with(path, ancestor);
    }
#endif

    return std::size(path) > std::size(ancestor) && tr_strv_starts_with(path, ancestor) && path[std::size(ancestor)] == '/';
}
} // namespace

std::optional<uint64_t> tr_parse_byte_size(std::string_view text)
{
    text = tr_strv_strip(text);
    if (std::empty(text))
    {
        return std::nullopt;
    }

    auto value = uint64_t{};
    auto any_digits = false;
    auto i = size_t{};
    for (; i < std::size(text) && std::isdigit(static_cast<unsigned char>(text[i])) != 0; ++i)
    {
        auto const digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
        {
            return std::nullopt;
        }

        value = value * 10U + digit;
        any_digits = true;
    }

    if (!any_digits)
    {
        return std::nullopt;
    }

    auto const suffix = tr_strlower(tr_strv_strip(text.substr(i)));

    auto multiplier = uint64_t{ 1 };
    if (std::empty(suffix) || suffix == "b")
    {
        multiplier = 1;
    }
    else if (suffix == "k" || suffix == "kb" || suffix == "kib")
    {
        multiplier = 1024ULL;
    }
    else if (suffix == "m" || suffix == "mb" || suffix == "mib")
    {
        multiplier = 1024ULL * 1024ULL;
    }
    else if (suffix == "g" || suffix == "gb" || suffix == "gib")
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    }
    else if (suffix == "t" || suffix == "tb" || suffix == "tib")
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    else
    {
        return std::nullopt;
    }

    if (value > std::numeric_limits<uint64_t>::max() / multiplier)
    {
        return std::nullopt;
    }

    return value * multiplier;
}

std::vector<tr_min_free_space_rule> tr_min_free_space_parse(std::vector<std::string> const& specs)
{
    auto rules = std::vector<tr_min_free_space_rule>{};
    rules.reserve(std::size(specs));

    for (auto const& spec : specs)
    {
        auto const spec_sv = std::string_view{ spec };
        auto const eq = spec_sv.rfind('=');
        if (eq == std::string_view::npos)
        {
            tr_logAddWarn(fmt::format("Ignoring min-free-space entry '{}': expected PATH=SIZE", spec));
            continue;
        }

        auto const path = tr_strv_strip(spec_sv.substr(0, eq));
        auto const bytes = tr_parse_byte_size(spec_sv.substr(eq + 1));
        if (std::empty(path) || !bytes)
        {
            tr_logAddWarn(fmt::format("Ignoring min-free-space entry '{}': invalid path or size", spec));
            continue;
        }

        rules.push_back({ normalize_path(path), *bytes });
    }

    return rules;
}

std::optional<uint64_t> tr_min_free_space_for(std::vector<tr_min_free_space_rule> const& rules, std::string_view dir)
{
    if (std::empty(rules))
    {
        return std::nullopt;
    }

    auto const path = normalize_path(dir);

    auto best = std::optional<uint64_t>{};
    auto best_len = size_t{};

    for (auto const& rule : rules)
    {
        if (is_ancestor_or_equal(rule.path, path) && std::size(rule.path) >= best_len)
        {
            best = rule.bytes;
            best_len = std::size(rule.path);
        }
    }

    return best;
}
