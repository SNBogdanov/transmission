// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "libtransmission/rpcimpl.h"
#include "libtransmission/variant.h"

struct tr_session;

class tr_script_runner
{
public:
    explicit tr_script_runner(std::string_view config_dir);
    ~tr_script_runner();

    tr_script_runner(tr_script_runner const&) = delete;
    tr_script_runner(tr_script_runner&&) = delete;
    tr_script_runner& operator=(tr_script_runner const&) = delete;
    tr_script_runner& operator=(tr_script_runner&&) = delete;

    void reload();

    [[nodiscard]] bool is_enabled() const;
    [[nodiscard]] bool is_restart_enabled() const;

    [[nodiscard]] tr_variant get_descriptor(tr_session const& session) const;

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> run(
        tr_session* session,
        tr_variant::Map const& args,
        tr_variant::Map& result);

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> status(
        tr_variant::Map const& args,
        tr_variant::Map& result);

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> cancel(
        tr_variant::Map const& args,
        tr_variant::Map& result);

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> restart_daemon(
        tr_variant::Map const& args,
        tr_variant::Map& result);

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> settings_get(tr_variant::Map& result);

    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> settings_set(
        tr_variant::Map const& args,
        tr_variant::Map& result);

private:
    struct Impl;
    std::unique_ptr<Impl> const impl_;
};
