// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <functional>
#include <map>
#include <string_view>

struct tr_error;

using tr_spawn_callback = std::function<void(int exit_status)>;

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    tr_error* error,
    tr_spawn_callback on_exit = {});
