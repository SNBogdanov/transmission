// This file Copyright В© Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/rpcimpl.h"
#include "libtransmission/script-runner.h"
#include "libtransmission/session.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrents.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"
#include "libtransmission/version.h"

using namespace std::literals;

namespace
{

constexpr auto ScriptRunnerSchema = "transgui-script-runner/v1"sv;
constexpr auto ScriptPayloadSchema = "transgui.script/v1"sv;
constexpr auto ScriptValuesSchema = "transgui-script-runner-values/v1"sv;
constexpr auto DefaultMaxOutputBytes = int64_t{ 1024 * 1024 };
constexpr auto MaxSelectionItems = int64_t{ 10000 };
constexpr auto MaxPayloadBytes = int64_t{ 8 * 1024 * 1024 };

[[nodiscard]] tr_quark q(std::string_view const key)
{
    return tr_quark_new(key);
}

[[nodiscard]] std::optional<std::string_view> get_string(tr_variant::Map const& map, std::string_view const key)
{
    return map.value_if<std::string_view>(q(key));
}

[[nodiscard]] std::optional<bool> get_bool(tr_variant::Map const& map, std::string_view const key)
{
    return map.value_if<bool>(q(key));
}

[[nodiscard]] std::optional<int64_t> get_int(tr_variant::Map const& map, std::string_view const key)
{
    return map.value_if<int64_t>(q(key));
}

[[nodiscard]] bool is_absolute(std::string_view const path)
{
    return !tr_sys_path_is_relative(path);
}

[[nodiscard]] std::string join_path(std::string_view const dir, std::string_view const subpath)
{
    if (std::empty(dir))
    {
        return std::string{ subpath };
    }

    auto ret = std::string{ dir };
    if (ret.back() != '/' && ret.back() != '\\')
    {
        ret += '/';
    }
    ret += subpath;
    return ret;
}

[[nodiscard]] std::string basename(std::string_view const path)
{
    auto const pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? std::string{ path } : std::string{ path.substr(pos + 1U) };
}

[[nodiscard]] std::string random_hex(size_t const n_bytes)
{
    auto bytes = std::vector<uint8_t>(n_bytes);
    tr_rand_buffer(std::data(bytes), std::size(bytes));
    auto out = std::string{};
    out.reserve(n_bytes * 2U);
    for (auto const byte : bytes)
    {
        fmt::format_to(std::back_inserter(out), "{:02x}", byte);
    }
    return out;
}

[[nodiscard]] std::string random_uuid()
{
    auto const hex = random_hex(16);
    return fmt::format(
        "{}-{}-{}-{}-{}",
        hex.substr(0, 8),
        hex.substr(8, 4),
        hex.substr(12, 4),
        hex.substr(16, 4),
        hex.substr(20, 12));
}

[[nodiscard]] std::string sha256_hex(std::string_view const input)
{
    return std::string{ tr_sha256_to_string(tr_sha256::digest(input)).sv() };
}

[[nodiscard]] std::string host_name()
{
#ifdef _WIN32
    auto buf = std::array<wchar_t, 256>{};
    auto size = DWORD{ static_cast<DWORD>(std::size(buf)) };
    if (GetComputerNameW(std::data(buf), &size) != 0)
    {
        return tr_win32_native_to_utf8(std::wstring_view{ std::data(buf), size });
    }
#else
    auto buf = std::array<char, 256>{};
    if (gethostname(std::data(buf), std::size(buf) - 1U) == 0)
    {
        return std::string{ std::data(buf) };
    }
#endif
    return "localhost"s;
}

[[nodiscard]] std::string rpc_url(tr_session const& session)
{
    auto const& settings = session.rpcServer().settings();
    if (!settings.is_enabled)
    {
        return {};
    }

    auto host = std::string{ settings.bind_address_str };
    if (std::empty(host) || host == "0.0.0.0" || host == "::")
    {
        host = "127.0.0.1";
    }
    if (host.find(':') != std::string::npos && !host.starts_with('['))
    {
        host = fmt::format("[{}]", host);
    }

    auto const port = settings.port.host();
    auto path = std::empty(settings.url) ? "/transmission/"s : std::string{ settings.url };
    if (!path.ends_with("/rpc"sv))
    {
        if (path.empty() || path.back() != '/')
        {
            path += '/';
        }
        path += "rpc";
    }
    return fmt::format("http://{}:{}{}", host, port, path);
}

[[nodiscard]] bool valid_context(std::string_view const context)
{
    return context == "torrent"sv || context == "file"sv || context == "system"sv;
}

struct ScriptParameter
{
    ScriptParameter() = default;
    ScriptParameter(ScriptParameter&&) noexcept = default;
    ScriptParameter& operator=(ScriptParameter&&) noexcept = default;

    std::string key;
    std::string name;
    std::string description;
    std::string type;
    std::string storage = "runtime";
    bool array = false;
    bool required = false;
    std::shared_ptr<tr_variant> default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> hosts;
};

struct ScriptDefinition
{
    ScriptDefinition() = default;
    ScriptDefinition(ScriptDefinition&&) noexcept = default;
    ScriptDefinition& operator=(ScriptDefinition&&) noexcept = default;

    std::string id;
    std::string name;
    std::string description;
    std::string group;
    int64_t order = 0;
    bool enabled = true;
    std::string platform = "any";
    std::vector<std::string> contexts;
    bool requires_selection = true;
    bool confirm = false;
    std::vector<ScriptParameter> parameters;
    std::string payload = std::string{ ScriptPayloadSchema };
    std::string stdin_format = "json";
    std::string executable;
    std::vector<std::string> args;
    std::string cwd;
    std::map<std::string, std::string> env;
    int64_t timeout_seconds = 300;
    int64_t max_output_bytes = DefaultMaxOutputBytes;
};

struct ExternalHost
{
    std::string id;
    std::string name;
    std::string description;
    bool enabled = true;
    std::string rpc_url;
    std::string netrc_machine;
};

struct RestartSettings
{
    bool enabled = false;
    std::string helper;
    std::vector<std::string> args;
    std::string cwd;
    int64_t timeout_seconds = 30;
    int64_t delay_seconds = 2;
    int64_t cooldown_seconds = 60;
};

struct Config
{
    bool enabled = false;
    int64_t max_concurrent = 1;
    int64_t queue_limit = 16;
    int64_t queue_limit_per_client = 4;
    int64_t max_concurrent_per_client = 1;
    int64_t job_retention_seconds = 3600;
    int64_t max_retained_jobs = 64;
    int64_t max_total_output_bytes = 64 * 1024 * 1024;
    int64_t max_output_bytes_default = DefaultMaxOutputBytes;
    int64_t max_selection_items = MaxSelectionItems;
    int64_t max_payload_bytes = MaxPayloadBytes;
    std::string rpc_netrc_file;
    std::vector<ExternalHost> external_hosts;
    RestartSettings restart;
    std::vector<ScriptDefinition> scripts;
};


[[nodiscard]] bool parse_string_array(tr_variant const& var, std::vector<std::string>& out)
{
    auto const* const vec = var.get_if<tr_variant::Vector>();
    if (vec == nullptr)
    {
        return false;
    }

    for (auto const& item : *vec)
    {
        auto const str = item.value_if<std::string_view>();
        if (!str || std::size(*str) > 4096U || str->find('\0') != std::string_view::npos)
        {
            return false;
        }
        out.emplace_back(*str);
    }
    return true;
}

[[nodiscard]] bool parse_parameter(tr_variant const& var, ScriptParameter& out)
{
    auto const* const map = var.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    auto const key = get_string(*map, "key");
    auto const name = get_string(*map, "name");
    auto const type = get_string(*map, "type");
    if (!key || !name || !type || std::empty(*key) || std::empty(*name))
    {
        return false;
    }

    out.key = *key;
    out.name = *name;
    out.description = get_string(*map, "description").value_or(""sv);
    out.type = *type;
    out.storage = get_string(*map, "storage").value_or("runtime"sv);
    out.array = get_bool(*map, "array").value_or(false);
    out.required = get_bool(*map, "required").value_or(false);

    if (out.type != "string"sv && out.type != "integer"sv && out.type != "number"sv && out.type != "boolean"sv &&
        out.type != "host"sv)
    {
        return false;
    }

    if (out.storage != "runtime"sv && out.storage != "host"sv)
    {
        return false;
    }

    if (auto const val = map->find(q("default")); val != std::end(*map))
    {
        out.default_value = std::make_shared<tr_variant>(val->second.clone());
    }

    if (auto const val = get_int(*map, "min"); val)
    {
        out.min = static_cast<double>(*val);
    }
    else if (auto const dval = map->value_if<double>(q("min")); dval)
    {
        out.min = *dval;
    }

    if (auto const val = get_int(*map, "max"); val)
    {
        out.max = static_cast<double>(*val);
    }
    else if (auto const dval = map->value_if<double>(q("max")); dval)
    {
        out.max = *dval;
    }

    if (auto const val = map->find(q("hosts")); val != std::end(*map))
    {
        if (out.type != "host"sv || !parse_string_array(val->second, out.hosts))
        {
            return false;
        }
    }

    if (out.type == "host"sv)
    {
        return !out.min.has_value() && !out.max.has_value();
    }
    if (out.type == "integer"sv || out.type == "number"sv)
    {
        return out.hosts.empty();
    }
    return out.hosts.empty() && !out.min.has_value() && !out.max.has_value();
}


[[nodiscard]] bool parse_script(tr_variant const& var, Config const& config, ScriptDefinition& out)
{
    auto const* const map = var.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    auto const id = get_string(*map, "id");
    auto const name = get_string(*map, "name");
    auto const executable = get_string(*map, "executable");
    auto const cwd = get_string(*map, "cwd");
    auto const payload = get_string(*map, "payload");
    auto const stdin_format = get_string(*map, "stdin");
    if (!id || !name || !executable || !cwd || !payload || !stdin_format)
    {
        return false;
    }
    if (*payload != ScriptPayloadSchema || *stdin_format != "json"sv)
    {
        return false;
    }
    if (!is_absolute(*executable) || !is_absolute(*cwd) || !tr_sys_path_exists(*cwd))
    {
        return false;
    }
    if (!tr_sys_path_exists(*executable))
    {
        return false;
    }

    out.id = *id;
    out.name = *name;
    out.description = get_string(*map, "description").value_or(""sv);
    out.group = get_string(*map, "group").value_or(""sv);
    out.order = get_int(*map, "order").value_or(0);
    out.enabled = get_bool(*map, "enabled").value_or(true);
    out.platform = get_string(*map, "platform").value_or("any"sv);
    out.requires_selection = get_bool(*map, "requires_selection").value_or(true);
    out.confirm = get_bool(*map, "confirm").value_or(false);
    out.executable = *executable;
    out.cwd = *cwd;
    out.timeout_seconds = get_int(*map, "timeout_seconds").value_or(300);
    out.max_output_bytes = get_int(*map, "max_output_bytes").value_or(config.max_output_bytes_default);

    if (out.platform != "any"sv && out.platform != "linux"sv && out.platform != "windows"sv)
    {
        return false;
    }
#ifdef _WIN32
    if (out.platform == "linux"sv)
    {
        return false;
    }
#endif

    auto const* const contexts = map->find_if<tr_variant::Vector>(q("contexts"));
    if (contexts == nullptr || std::empty(*contexts))
    {
        return false;
    }
    for (auto const& item : *contexts)
    {
        auto const context = item.value_if<std::string_view>();
        if (!context || !valid_context(*context))
        {
            return false;
        }
        out.contexts.emplace_back(*context);
    }
    std::ranges::sort(out.contexts);
    if (std::adjacent_find(out.contexts.begin(), out.contexts.end()) != out.contexts.end())
    {
        return false;
    }
    if (std::ranges::find(out.contexts, "system"sv) != out.contexts.end())
    {
        if (std::size(out.contexts) != 1U || out.requires_selection)
        {
            return false;
        }
    }

    if (auto const val = map->find_if<tr_variant::Vector>(q("args")); val != nullptr)
    {
        for (auto const& item : *val)
        {
            auto const arg = item.value_if<std::string_view>();
            if (!arg || arg->find('\0') != std::string_view::npos)
            {
                return false;
            }
            out.args.emplace_back(*arg);
        }
    }

    if (auto const* const roles = map->find_if<tr_variant::Map>(q("env")); roles != nullptr)
    {
        for (auto const& [env_key, env_val] : *roles)
        {
            auto const value = env_val.value_if<std::string_view>();
            if (!value || value->find('\0') != std::string_view::npos)
            {
                return false;
            }
            out.env.emplace(std::string{ tr_quark_get_string_view(env_key) }, std::string{ *value });
        }
    }

    if (auto const* const params = map->find_if<tr_variant::Vector>(q("parameters")); params != nullptr)
    {
        if (std::size(*params) > 64U)
        {
            return false;
        }
        for (auto const& param_var : *params)
        {
            auto param = ScriptParameter{};
            if (!parse_parameter(param_var, param))
            {
                return false;
            }
            if (std::ranges::any_of(out.parameters, [&param](auto const& item) { return item.key == param.key; }))
            {
                return false;
            }
            if (param.required && param.default_value)
            {
                return false;
            }
            if (param.type == "host"sv)
            {
                if (param.hosts.empty())
                {
                    for (auto const& host : config.external_hosts)
                    {
                        if (host.enabled)
                        {
                            param.hosts.emplace_back(host.id);
                        }
                    }
                }
                if (std::empty(param.hosts))
                {
                    return false;
                }
                for (auto const& host_id : param.hosts)
                {
                    auto const host = std::ranges::find_if(
                        config.external_hosts,
                        [&host_id](auto const& item) { return item.id == host_id; });
                    if (host == std::end(config.external_hosts) || !host->enabled)
                    {
                        return false;
                    }
                }
            }
            out.parameters.emplace_back(std::move(param));
        }
    }

    return true;
}


[[nodiscard]] bool parse_external_host(tr_variant const& var, ExternalHost& out)
{
    auto const* const map = var.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    auto const id = get_string(*map, "id");
    auto const rpc_url = get_string(*map, "rpc_url");
    if (!id || !rpc_url || std::empty(*id) || std::empty(*rpc_url))
    {
        return false;
    }
    if (rpc_url->find("://"sv) == std::string_view::npos)
    {
        return false;
    }

    out.id = *id;
    out.name = get_string(*map, "name").value_or(*id);
    out.description = get_string(*map, "description").value_or(""sv);
    out.enabled = get_bool(*map, "enabled").value_or(true);
    out.rpc_url = *rpc_url;
    out.netrc_machine = get_string(*map, "netrc_machine").value_or(*id);
    return true;
}

[[nodiscard]] bool parse_restart(tr_variant const& var, RestartSettings& out)
{
    auto const* const map = var.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    out.enabled = get_bool(*map, "enabled").value_or(false);
    if (!out.enabled)
    {
        return true;
    }

    auto const helper = get_string(*map, "helper");
    auto const cwd = get_string(*map, "cwd");
    if (!helper || !cwd || !is_absolute(*helper) || !is_absolute(*cwd) || !tr_sys_path_exists(*helper) ||
        !tr_sys_path_exists(*cwd))
    {
        return false;
    }
#ifdef _WIN32
    auto const lower = tr_strlower(*helper);
    if (!tr_strv_ends_with(lower, ".exe"sv))
    {
        return false;
    }
#endif
    out.helper = *helper;
    out.cwd = *cwd;
    out.timeout_seconds = get_int(*map, "timeout_seconds").value_or(30);
    out.delay_seconds = get_int(*map, "delay_seconds").value_or(2);
    out.cooldown_seconds = get_int(*map, "cooldown_seconds").value_or(60);
    if (out.timeout_seconds < 1 || out.timeout_seconds > 300 || out.delay_seconds < 0 || out.delay_seconds > 300 ||
        out.cooldown_seconds < 0 || out.cooldown_seconds > 86400)
    {
        return false;
    }

    if (auto const* const args = map->find_if<tr_variant::Vector>(q("args")); args != nullptr)
    {
        for (auto const& item : *args)
        {
            auto const arg = item.value_if<std::string_view>();
            if (!arg || arg->find('\0') != std::string_view::npos)
            {
                return false;
            }
            out.args.emplace_back(*arg);
        }
    }
    return true;
}


[[nodiscard]] bool load_config(std::string_view const path, Config& out)
{
    auto const var = tr_variant_serde::json().parse_file(path);
    if (!var)
    {
        return false;
    }

    auto const* const map = var->get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return false;
    }

    auto const schema = get_string(*map, "schema");
    if (schema != ScriptRunnerSchema)
    {
        return false;
    }

    out.enabled = get_bool(*map, "enabled").value_or(true);
    out.max_concurrent = get_int(*map, "max_concurrent").value_or(1);
    out.queue_limit = get_int(*map, "queue_limit").value_or(16);
    out.queue_limit_per_client = get_int(*map, "queue_limit_per_client").value_or(4);
    out.max_concurrent_per_client = get_int(*map, "max_concurrent_per_client").value_or(1);
    out.job_retention_seconds = get_int(*map, "job_retention_seconds").value_or(3600);
    out.max_retained_jobs = get_int(*map, "max_retained_jobs").value_or(64);
    out.max_total_output_bytes = get_int(*map, "max_total_output_bytes").value_or(64 * 1024 * 1024);
    out.max_output_bytes_default = get_int(*map, "max_output_bytes_default").value_or(DefaultMaxOutputBytes);
    out.max_selection_items = get_int(*map, "max_selection_items").value_or(MaxSelectionItems);
    out.max_payload_bytes = get_int(*map, "max_payload_bytes").value_or(MaxPayloadBytes);
    out.rpc_netrc_file = get_string(*map, "rpc_netrc_file").value_or(""sv);

    if (out.max_concurrent < 1 || out.max_concurrent > 16 || out.queue_limit < 1 || out.queue_limit > 256 ||
        out.queue_limit_per_client < 1 || out.queue_limit_per_client > out.queue_limit || out.max_concurrent_per_client < 0 ||
        out.max_concurrent_per_client > out.max_concurrent || out.max_output_bytes_default < 1 ||
        out.max_selection_items < 1 || out.max_payload_bytes < 1024)
    {
        return false;
    }

    if (auto const* const hosts = map->find_if<tr_variant::Vector>(q("external_hosts")); hosts != nullptr)
    {
        if (std::size(*hosts) > 64U)
        {
            return false;
        }
        for (auto const& host_var : *hosts)
        {
            auto host = ExternalHost{};
            if (!parse_external_host(host_var, host))
            {
                continue;
            }
            if (std::ranges::any_of(out.external_hosts, [&host](auto const& item) { return item.id == host.id; }))
            {
                continue;
            }
            out.external_hosts.emplace_back(std::move(host));
        }
    }

    auto const restart_iter = map->find(q("restart_daemon"));
    if (restart_iter != std::end(*map))
    {
        auto const* const restart = restart_iter->second.get_if<tr_variant::Map>();
        if (restart == nullptr)
        {
            return false;
        }

        out.restart.enabled = get_bool(*restart, "enabled").value_or(false);
        if (out.restart.enabled)
        {
            auto const helper = get_string(*restart, "helper");
            auto const cwd = get_string(*restart, "cwd");
            if (!helper || !cwd || !is_absolute(*helper) || !is_absolute(*cwd) || !tr_sys_path_exists(*helper) ||
                !tr_sys_path_exists(*cwd))
            {
                return false;
            }
#ifdef _WIN32
            if (!tr_strv_ends_with(tr_strlower(*helper), ".exe"sv))
            {
                return false;
            }
#endif
            out.restart.helper = *helper;
            out.restart.cwd = *cwd;
            out.restart.timeout_seconds = get_int(*restart, "timeout_seconds").value_or(30);
            out.restart.delay_seconds = get_int(*restart, "delay_seconds").value_or(2);
            out.restart.cooldown_seconds = get_int(*restart, "cooldown_seconds").value_or(60);
            if (auto const* const args = restart->find_if<tr_variant::Vector>(q("args")); args != nullptr)
            {
                for (auto const& item : *args)
                {
                    auto const arg = item.value_if<std::string_view>();
                    if (!arg || arg->find('\0') != std::string_view::npos)
                    {
                        return false;
                    }
                    out.restart.args.emplace_back(*arg);
                }
            }
        }
    }

    if (auto const* const scripts = map->find_if<tr_variant::Vector>(q("scripts")); scripts != nullptr)
    {
        if (std::size(*scripts) > 256U)
        {
            return false;
        }
        out.scripts.reserve(std::size(*scripts));
        for (auto const& script_var : *scripts)
        {
            auto script = ScriptDefinition{};
            if (!parse_script(script_var, out, script))
            {
                continue;
            }
            if (std::ranges::any_of(out.scripts, [&script](auto const& item) { return item.id == script.id; }))
            {
                continue;
            }
            out.scripts.emplace_back(std::move(script));
        }
    }

    return true;
}
[[nodiscard]] bool is_script_available(ScriptDefinition const& script)
{
#ifdef _WIN32
    return script.platform == "any"sv || script.platform == "windows"sv;
#else
    return script.platform == "any"sv || script.platform == "linux"sv;
#endif
}

[[nodiscard]] tr_variant::Map make_parameter_descriptor(ScriptParameter const& param)
{
    auto map = tr_variant::Map{};
    map.try_emplace(q("key"), param.key);
    map.try_emplace(q("name"), param.name);
    map.try_emplace(q("description"), param.description);
    map.try_emplace(q("type"), param.type);
    map.try_emplace(q("storage"), param.storage);
    map.try_emplace(q("array"), param.array);
    map.try_emplace(q("required"), param.required);
    if (param.default_value)
    {
        map.try_emplace(q("default"), param.default_value->clone());
    }
    if (param.min)
    {
        map.try_emplace(q("min"), *param.min);
    }
    if (param.max)
    {
        map.try_emplace(q("max"), *param.max);
    }
    if (param.type == "host"sv)
    {
        auto hosts = tr_variant::Vector{};
        hosts.reserve(std::size(param.hosts));
        for (auto const& id : param.hosts)
        {
            hosts.emplace_back(id);
        }
        map.try_emplace(q("hosts"), tr_variant{ std::move(hosts) });
    }
    return map;
}

[[nodiscard]] tr_variant::Map make_script_descriptor(ScriptDefinition const& script)
{
    auto map = tr_variant::Map{};
    map.try_emplace(q("id"), script.id);
    map.try_emplace(q("name"), script.name);
    map.try_emplace(q("description"), script.description);
    map.try_emplace(q("group"), script.group);
    map.try_emplace(q("order"), script.order);
    map.try_emplace(q("platform"), script.platform);
    auto contexts = tr_variant::Vector{};
    contexts.reserve(std::size(script.contexts));
    for (auto const& context : script.contexts)
    {
        contexts.emplace_back(context);
    }
    map.try_emplace(q("contexts"), tr_variant{ std::move(contexts) });
    map.try_emplace(q("requires_selection"), script.requires_selection);
    map.try_emplace(q("confirm"), script.confirm);
    auto params = tr_variant::Vector{};
    params.reserve(std::size(script.parameters));
    for (auto const& param : script.parameters)
    {
        params.emplace_back(make_parameter_descriptor(param));
    }
    map.try_emplace(q("parameters"), tr_variant{ std::move(params) });
    map.try_emplace(q("payload"), script.payload);
    map.try_emplace(q("stdin"), script.stdin_format);
    map.try_emplace(q("timeout_seconds"), script.timeout_seconds);
    map.try_emplace(q("max_output_bytes"), script.max_output_bytes);
    return map;
}

[[nodiscard]] tr_variant::Vector clone_vector(tr_variant::Vector const& input)
{
    auto output = tr_variant::Vector{};
    output.reserve(std::size(input));
    for (auto const& item : input)
    {
        output.emplace_back(item.clone());
    }
    return output;
}

[[nodiscard]] tr_variant::Map make_external_host_descriptor(ExternalHost const& host)
{
    auto map = tr_variant::Map{};
    map.try_emplace(q("id"), host.id);
    map.try_emplace(q("name"), host.name);
    map.try_emplace(q("description"), host.description);
    return map;
}

struct ProcessRuntime;

struct Job
{
    enum class State
    {
        Queued,
        Running,
        Finished,
        Failed,
        Canceled,
        TimedOut,
    };

    std::string id;
    std::string token_hash;
    std::string script_id;
    State state = State::Queued;
    int64_t exit_code = 0;
    bool has_exit_code = false;
    int64_t term_signal = 0;
    bool has_term_signal = false;
    std::string error;
    std::string stdout_buf;
    std::string stderr_buf;
    int64_t stdout_total = 0;
    int64_t stderr_total = 0;
    int64_t stdout_stored = 0;
    int64_t stderr_stored = 0;
    int64_t max_output = DefaultMaxOutputBytes;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool timed_out = false;
    bool canceled = false;
    int64_t queued_at = 0;
    int64_t started_at = 0;
    int64_t finished_at = 0;

    std::mutex mutex;
    std::shared_ptr<ProcessRuntime> process;
};

struct ProcessRuntime
{
    std::mutex mutex;
    std::atomic<bool> cancel_requested = false;
    std::atomic<bool> timeout_requested = false;
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    std::thread stdin_thread;
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::thread waiter;
#else
    pid_t pid = -1;
    int stdin_read = -1;
    int stdin_write = -1;
    int stdout_read = -1;
    int stdout_write = -1;
    int stderr_read = -1;
    int stderr_write = -1;
    std::thread stdin_thread;
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::thread waiter;
#endif
    ~ProcessRuntime();
};

#ifdef _WIN32
void close_handle(HANDLE& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}

ProcessRuntime::~ProcessRuntime()
{
    cancel_requested = true;
    if (job != nullptr)
    {
        TerminateJobObject(job, 1U);
    }
    if (process != nullptr)
    {
        TerminateProcess(process, 1U);
    }
    if (waiter.joinable())
    {
        waiter.join();
    }
    if (stdin_thread.joinable())
    {
        stdin_thread.join();
    }
    if (stdout_thread.joinable())
    {
        stdout_thread.join();
    }
    if (stderr_thread.joinable())
    {
        stderr_thread.join();
    }
    close_handle(stdin_read);
    close_handle(stdin_write);
    close_handle(stdout_read);
    close_handle(stdout_write);
    close_handle(stderr_read);
    close_handle(stderr_write);
    close_handle(process);
    close_handle(job);
}
#else
void close_fd(int& fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

ProcessRuntime::~ProcessRuntime()
{
    cancel_requested = true;
    if (pid > 0)
    {
        kill(-pid, SIGKILL);
    }
    if (waiter.joinable())
    {
        waiter.join();
    }
    if (stdin_thread.joinable())
    {
        stdin_thread.join();
    }
    if (stdout_thread.joinable())
    {
        stdout_thread.join();
    }
    if (stderr_thread.joinable())
    {
        stderr_thread.join();
    }
    close_fd(stdin_read);
    close_fd(stdin_write);
    close_fd(stdout_read);
    close_fd(stdout_write);
    close_fd(stderr_read);
    close_fd(stderr_write);
}
#endif

void log_job_finished(Job const& job, std::string_view const client_id)
{
    auto const duration = job.finished_at >= job.started_at ? job.finished_at - job.started_at : int64_t{};
    auto const script_id = std::string_view{ job.script_id };

    switch (job.state)
    {
    case Job::State::Finished:
        tr_logAddInfo(fmt::format(
            "script runner: job {} script '{}' client '{}' completed successfully in {}s",
            job.id,
            script_id,
            client_id,
            duration));
        break;

    case Job::State::Canceled:
        tr_logAddInfo(fmt::format(
            "script runner: job {} script '{}' client '{}' canceled after {}s",
            job.id,
            script_id,
            client_id,
            duration));
        break;

    case Job::State::TimedOut:
        tr_logAddError(fmt::format(
            "script runner: job {} script '{}' client '{}' timed out after {}s (signal={})",
            job.id,
            script_id,
            client_id,
            duration,
            job.has_term_signal ? job.term_signal : 0));
        break;

    case Job::State::Failed:
        tr_logAddError(fmt::format(
            "script runner: job {} script '{}' client '{}' failed after {}s (exit={}, signal={}, error={})",
            job.id,
            script_id,
            client_id,
            duration,
            job.has_exit_code ? job.exit_code : 0,
            job.has_term_signal ? job.term_signal : 0,
            job.error));
        break;

    case Job::State::Queued:
    case Job::State::Running:
        break;
    }
}
[[nodiscard]] bool is_host_allowed(ScriptParameter const& param, std::string_view const id)
{
    return std::ranges::find(param.hosts, id) != std::end(param.hosts);
}

[[nodiscard]] bool validate_scalar(
    ScriptParameter const& param,
    tr_variant const& value,
    std::map<std::string, ExternalHost const*>& selected_hosts,
    std::map<std::string, ExternalHost const*> const& all_hosts,
    std::string& error)
{
    if (param.type == "string"sv)
    {
        auto const str = value.value_if<std::string_view>();
        if (!str || std::size(*str) > 4096U || str->find('\0') != std::string_view::npos)
        {
            error = "invalid script parameters";
            return false;
        }
        return true;
    }

    if (param.type == "boolean"sv)
    {
        if (!value.value_if<bool>())
        {
            error = "invalid script parameters";
            return false;
        }
        return true;
    }

    if (param.type == "integer"sv || param.type == "number"sv)
    {
        auto number = std::optional<double>{};
        if (auto const i = value.value_if<int64_t>(); i)
        {
            number = static_cast<double>(*i);
        }
        else if (auto const d = value.value_if<double>(); d)
        {
            number = *d;
        }
        else
        {
            error = "invalid script parameters";
            return false;
        }
        if (param.type == "integer"sv && !value.value_if<int64_t>())
        {
            error = "invalid script parameters";
            return false;
        }
        if ((param.min && *number < *param.min) || (param.max && *number > *param.max))
        {
            error = "invalid script parameters";
            return false;
        }
        return true;
    }

    if (param.type == "host"sv)
    {
        auto const id = value.value_if<std::string_view>();
        if (!id || !is_host_allowed(param, *id))
        {
            error = "invalid script parameters";
            return false;
        }
        auto const host = all_hosts.find(std::string{ *id });
        if (host == std::end(all_hosts))
        {
            error = "invalid script parameters";
            return false;
        }
        selected_hosts.insert_or_assign(host->first, host->second);
        return true;
    }

    error = "invalid script parameters";
    return false;
}

[[nodiscard]] bool validate_parameter(
    ScriptParameter const& param,
    tr_variant const& value,
    std::map<std::string, ExternalHost const*>& selected_hosts,
    std::map<std::string, ExternalHost const*> const& all_hosts,
    std::string& error)
{
    if (!param.array)
    {
        return validate_scalar(param, value, selected_hosts, all_hosts, error);
    }

    auto const* const vec = value.get_if<tr_variant::Vector>();
    if (vec == nullptr || std::size(*vec) > 256U || (param.required && std::empty(*vec)))
    {
        error = "invalid script parameters";
        return false;
    }
    for (auto const& item : *vec)
    {
        if (!validate_scalar(param, item, selected_hosts, all_hosts, error))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool resolve_parameters(
    Config const& config,
    ScriptDefinition const& script,
    tr_variant::Map const* input,
    std::map<std::string, std::shared_ptr<tr_variant>> const* stored,
    bool values_available,
    tr_variant::Map& output,
    std::map<std::string, ExternalHost const*>& selected_hosts,
    std::string& error)
{
    auto all_hosts = std::map<std::string, ExternalHost const*>{};
    for (auto const& host : config.external_hosts)
    {
        if (host.enabled)
        {
            all_hosts.emplace(host.id, &host);
        }
    }

    auto const empty_input = tr_variant::Map{};
    auto const& source = input != nullptr ? *input : empty_input;
    for (auto const& param : script.parameters)
    {
        if (param.storage == "host"sv)
        {
            if (!values_available)
            {
                error = "script settings unavailable";
                return false;
            }

            auto const* const stored_value = stored != nullptr && stored->find(param.key) != std::end(*stored)
                ? stored->find(param.key)->second.get()
                : nullptr;
            if (stored_value != nullptr)
            {
                auto value = stored_value->clone();
                if (validate_parameter(param, value, selected_hosts, all_hosts, error))
                {
                    output.try_emplace(q(param.key), std::move(value));
                }
                else if (param.required)
                {
                    error = "script settings incomplete";
                    return false;
                }
            }
            else if (param.default_value)
            {
                auto value = param.default_value->clone();
                if (!validate_parameter(param, value, selected_hosts, all_hosts, error))
                {
                    error = "script settings incomplete";
                    return false;
                }
                output.try_emplace(q(param.key), std::move(value));
            }
            else if (param.required)
            {
                error = "script settings incomplete";
                return false;
            }
            continue;
        }

        auto const iter = source.find(q(param.key));
        if (iter == std::end(source))
        {
            if (param.required)
            {
                error = "invalid script parameters";
                return false;
            }
            if (param.default_value)
            {
                auto default_value = param.default_value->clone();
                if (!validate_parameter(param, default_value, selected_hosts, all_hosts, error))
                {
                    return false;
                }
                output.try_emplace(q(param.key), std::move(default_value));
            }
            continue;
        }

        if (!validate_parameter(param, iter->second, selected_hosts, all_hosts, error))
        {
            return false;
        }
        output.try_emplace(q(param.key), iter->second.clone());
    }

    if (input != nullptr)
    {
        for (auto const& [key, value] : *input)
        {
            auto const key_view = tr_quark_get_string_view(key);
            auto const param = std::ranges::find_if(
                script.parameters,
                [key_view](auto const& item) { return item.key == key_view; });
            if (param == std::end(script.parameters) || param->storage != "runtime"sv)
            {
                error = "invalid script parameters";
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] bool build_execution_payload(
    tr_session* session,
    Config const& config,
    ScriptDefinition const& script,
    tr_variant::Map const& parameters,
    std::map<std::string, ExternalHost const*> const& selected_hosts,
    tr_variant::Map const& request,
    std::string& payload_out,
    std::string& error)
{
    auto lock = session->unique_lock();

    auto const context = request.value_if<std::string_view>(q("context")).value_or(""sv);
    if (!valid_context(context))
    {
        error = "invalid script payload";
        return false;
    }

    auto const* const selection = request.find_if<tr_variant::Map>(q("selection"));
    auto const* const hashes = selection != nullptr ? selection->find_if<tr_variant::Vector>(q("hashes")) : nullptr;
    auto const* const files = selection != nullptr ? selection->find_if<tr_variant::Vector>(q("files")) : nullptr;
    auto const host = request.value_if<std::string_view>(q("host")).value_or(""sv);

    if (context == "system"sv)
    {
        if ((hashes != nullptr && !std::empty(*hashes)) || (files != nullptr && !std::empty(*files)))
        {
            error = "invalid script payload";
            return false;
        }
    }
    else if (context == "torrent"sv && files != nullptr && !std::empty(*files))
    {
        error = "invalid script payload";
        return false;
    }
    else if (context == "file"sv && hashes != nullptr && !std::empty(*hashes))
    {
        error = "invalid script payload";
        return false;
    }

    auto owners = std::vector<tr_torrent*>{};
    auto selected_files = std::set<std::pair<tr_torrent*, tr_file_index_t>>{};
    auto seen_hashes = std::set<std::string>{};

    auto find_torrent = [session](std::string_view const hash) -> tr_torrent*
    {
        auto const digest = tr_sha1_from_string(hash);
        return digest ? session->torrents().get(*digest) : nullptr;
    };

    auto add_owner = [&owners](tr_torrent* tor)
    {
        if (std::ranges::find(owners, tor) == std::end(owners))
        {
            owners.emplace_back(tor);
        }
    };

    if (context == "torrent"sv && hashes != nullptr)
    {
        for (auto const& hash_var : *hashes)
        {
            auto const hash = hash_var.value_if<std::string_view>();
            if (!hash || std::size(*hash) != 40U)
            {
                error = "invalid script payload";
                return false;
            }
            auto const lower = tr_strlower(*hash);
            if (!seen_hashes.emplace(lower).second)
            {
                continue;
            }
            auto* const tor = find_torrent(lower);
            if (tor == nullptr || !tor->has_metainfo())
            {
                error = "invalid script payload";
                return false;
            }
            add_owner(tor);
        }
    }
    else if (context == "file"sv && files != nullptr)
    {
        for (auto const& file_var : *files)
        {
            auto const* const file = file_var.get_if<tr_variant::Map>();
            auto const hash = file != nullptr ? get_string(*file, "hash") : std::nullopt;
            auto const file_index = file != nullptr ? get_int(*file, "file_index") : std::nullopt;
            if (!hash || std::size(*hash) != 40U || !file_index || *file_index < 0)
            {
                error = "invalid script payload";
                return false;
            }
            auto const lower = tr_strlower(*hash);
            auto* const tor = find_torrent(lower);
            if (tor == nullptr || !tor->has_metainfo() || static_cast<uint64_t>(*file_index) >= tor->file_count())
            {
                error = "invalid script payload";
                return false;
            }
            add_owner(tor);
            selected_files.emplace(tor, static_cast<tr_file_index_t>(*file_index));
        }
    }

    if (script.requires_selection && std::empty(owners))
    {
        error = "script requires a selection";
        return false;
    }

    auto torrents_vec = tr_variant::Vector{};
    auto torrent_id_vec = tr_variant::Vector{};
    auto name_vec = tr_variant::Vector{};
    auto hash_vec = tr_variant::Vector{};
    auto download_dir_vec = tr_variant::Vector{};
    torrents_vec.reserve(std::size(owners));
    torrent_id_vec.reserve(std::size(owners));
    name_vec.reserve(std::size(owners));
    hash_vec.reserve(std::size(owners));
    download_dir_vec.reserve(std::size(owners));

    for (auto* const tor : owners)
    {
        auto torrent_map = tr_variant::Map{ 4U };
        torrent_map.try_emplace(q("torrent_id"), static_cast<int64_t>(tor->id()));
        torrent_map.try_emplace(q("name"), tor->name());
        torrent_map.try_emplace(q("hash"), tor->info_hash_string().sv());
        torrent_map.try_emplace(q("download_dir"), tor->download_dir().sv());
        torrents_vec.emplace_back(std::move(torrent_map));
        torrent_id_vec.emplace_back(static_cast<int64_t>(tor->id()));
        name_vec.emplace_back(tor->name());
        hash_vec.emplace_back(tor->info_hash_string().sv());
        download_dir_vec.emplace_back(tor->download_dir().sv());
    }

    auto files_vec = tr_variant::Vector{};
    if (context == "file"sv)
    {
        for (auto* const tor : owners)
        {
            auto const n_files = tor->file_count();
            files_vec.reserve(std::size(files_vec) + n_files);
            for (tr_file_index_t idx = 0U; idx != n_files; ++idx)
            {
                auto const file = tr_torrentFile(tor, idx);
                auto file_map = tr_variant::Map{ 10U };
                file_map.try_emplace(q("torrent_id"), static_cast<int64_t>(tor->id()));
                file_map.try_emplace(q("hash"), tor->info_hash_string().sv());
                file_map.try_emplace(q("file_index"), static_cast<int64_t>(idx));
                file_map.try_emplace(q("relative_path"), file.name);
                file_map.try_emplace(q("path"), join_path(tor->download_dir().sv(), file.name));
                file_map.try_emplace(q("name"), basename(file.name));
                file_map.try_emplace(q("size"), static_cast<int64_t>(file.length));
                file_map.try_emplace(q("wanted"), file.wanted);
                file_map.try_emplace(q("priority"), static_cast<int64_t>(file.priority));
                file_map.try_emplace(q("selected"), selected_files.contains({ tor, idx }));
                files_vec.emplace_back(std::move(file_map));
            }
        }
    }

    auto external_hosts_map = tr_variant::Map{};
    for (auto const& [id, host] : selected_hosts)
    {
        auto host_map = tr_variant::Map{ 3U };
        host_map.try_emplace(q("name"), host->name);
        host_map.try_emplace(q("rpc_url"), host->rpc_url);
        host_map.try_emplace(q("netrc_machine"), host->netrc_machine);
        external_hosts_map.try_emplace(q(id), tr_variant{ std::move(host_map) });
    }

    auto payload = tr_variant::Map{};
    payload.try_emplace(q("schema"), std::string{ ScriptPayloadSchema });
    payload.try_emplace(q("context"), context);
    payload.try_emplace(q("host"), host);
    payload.try_emplace(q("parameters"), parameters.clone());
    payload.try_emplace(q("external_hosts"), tr_variant{ std::move(external_hosts_map) });

    auto server_map = tr_variant::Map{ 3U };
    server_map.try_emplace(q("host"), host_name());
#ifdef _WIN32
    server_map.try_emplace(q("os"), "windows"sv);
#else
    server_map.try_emplace(q("os"), "linux"sv);
#endif
    server_map.try_emplace(q("daemon_version"), LONG_VERSION_STRING);
    payload.try_emplace(q("server"), tr_variant{ std::move(server_map) });

    auto const url = rpc_url(*session);
    auto rpc_map = tr_variant::Map{ 2U };
    if (std::empty(url))
    {
        rpc_map.try_emplace(q("url"), tr_variant{ std::nullptr_t{} });
    }
    else
    {
        rpc_map.try_emplace(q("url"), url);
    }
    if (std::empty(config.rpc_netrc_file))
    {
        rpc_map.try_emplace(q("netrc"), tr_variant{ std::nullptr_t{} });
    }
    else
    {
        rpc_map.try_emplace(q("netrc"), config.rpc_netrc_file);
    }
    payload.try_emplace(q("rpc"), tr_variant{ std::move(rpc_map) });

    auto script_map = tr_variant::Map{ 2U };
    script_map.try_emplace(q("id"), script.id);
    script_map.try_emplace(q("name"), script.name);
    payload.try_emplace(q("script"), tr_variant{ std::move(script_map) });

    payload.try_emplace(q("torrents"), tr_variant{ std::move(torrents_vec) });
    payload.try_emplace(q("torrent_id"), tr_variant{ std::move(torrent_id_vec) });
    payload.try_emplace(q("name"), tr_variant{ std::move(name_vec) });
    payload.try_emplace(q("hash"), tr_variant{ std::move(hash_vec) });
    payload.try_emplace(q("download_dir"), tr_variant{ std::move(download_dir_vec) });
    payload.try_emplace(q("files"), tr_variant{ std::move(files_vec) });

    auto const serialized = tr_variant_serde::json().compact().to_string(tr_variant{ std::move(payload) });
    if (static_cast<int64_t>(std::size(serialized)) > config.max_payload_bytes)
    {
        error = "script payload is too large";
        return false;
    }
    payload_out = serialized;
    return true;
}

#ifdef _WIN32
[[nodiscard]] std::wstring quote_argument(std::string_view const argument)
{
    auto const native = tr_win32_utf8_to_native(argument);
    if (std::empty(native))
    {
        return L"\"\"";
    }
    if (native.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return native;
    }

    auto out = std::wstring{ L"\"" };
    auto backslashes = size_t{};
    for (auto const ch : native)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (ch == L'"')
        {
            out.append(backslashes * 2U + 1U, L'\\');
            out.push_back(ch);
        }
        else
        {
            out.append(backslashes, L'\\');
            out.push_back(ch);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2U, L'\\');
    out.push_back(L'"');
    return out;
}

[[nodiscard]] bool append_environment(
    std::map<std::string, std::string> const& env,
    std::vector<wchar_t>& out)
{
    auto full_env = std::map<std::wstring, std::wstring, std::less<>>{};
    for (auto const* const name : { L"SystemRoot", L"SystemDrive", L"PATH", L"TEMP", L"TMP", L"USERPROFILE" })
    {
        auto buf = std::array<wchar_t, 32767>{};
        auto const size = GetEnvironmentVariableW(name, std::data(buf), static_cast<DWORD>(std::size(buf)));
        if (size > 0 && size < std::size(buf))
        {
            full_env.emplace(std::wstring{ name }, std::wstring{ std::data(buf), size });
        }
    }
    for (auto const& [key, value] : env)
    {
        full_env.insert_or_assign(tr_win32_utf8_to_native(key), tr_win32_utf8_to_native(value));
    }

    for (auto const& [key, value] : full_env)
    {
        auto const line = key + L'=' + value;
        out.insert(std::end(out), std::begin(line), std::end(line));
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return true;
}

#endif

#ifdef _WIN32
void read_pipe(HANDLE pipe, Job* job, bool is_stdout)
{
    auto buf = std::array<char, 8192>{};
    for (;;)
    {
        auto n_read = DWORD{};
        if (ReadFile(pipe, std::data(buf), static_cast<DWORD>(std::size(buf)), &n_read, nullptr) == 0 || n_read == 0)
        {
            break;
        }

        auto lock = std::scoped_lock{ job->mutex };
        auto& total = is_stdout ? job->stdout_total : job->stderr_total;
        auto& stored = is_stdout ? job->stdout_stored : job->stderr_stored;
        auto& truncated = is_stdout ? job->stdout_truncated : job->stderr_truncated;
        auto& output = is_stdout ? job->stdout_buf : job->stderr_buf;
        total += static_cast<int64_t>(n_read);
        auto const room = std::max<int64_t>(0, job->max_output - stored);
        auto const to_store = std::min<int64_t>(room, static_cast<int64_t>(n_read));
        if (to_store > 0)
        {
            output.append(std::data(buf), static_cast<size_t>(to_store));
            stored += to_store;
        }
        if (to_store < static_cast<int64_t>(n_read))
        {
            truncated = true;
        }
    }
}
#else
void read_pipe(int fd, Job* job, bool is_stdout)
{
    auto buf = std::array<char, 8192>{};
    for (;;)
    {
        auto const n_read = read(fd, std::data(buf), std::size(buf));
        if (n_read <= 0)
        {
            if (n_read < 0 && errno == EINTR)
            {
                continue;
            }
            break;
        }

        auto lock = std::scoped_lock{ job->mutex };
        auto& total = is_stdout ? job->stdout_total : job->stderr_total;
        auto& stored = is_stdout ? job->stdout_stored : job->stderr_stored;
        auto& truncated = is_stdout ? job->stdout_truncated : job->stderr_truncated;
        auto& output = is_stdout ? job->stdout_buf : job->stderr_buf;
        total += static_cast<int64_t>(n_read);
        auto const room = std::max<int64_t>(0, job->max_output - stored);
        auto const to_store = std::min<int64_t>(room, static_cast<int64_t>(n_read));
        if (to_store > 0)
        {
            output.append(std::data(buf), static_cast<size_t>(to_store));
            stored += to_store;
        }
        if (to_store < static_cast<int64_t>(n_read))
        {
            truncated = true;
        }
    }
}
#endif

struct Impl
{
    explicit Impl(std::string_view config_dir_in)
        : config_dir{ config_dir_in }
    {
        reload();
    }

    ~Impl()
    {
        cancel_all();
    }

    void reload();
    void cancel_all();
    [[nodiscard]] bool start_job(
        tr_session* session,
        ScriptDefinition const& script,
        tr_variant::Map const& parameters,
        tr_variant::Map const& external_hosts,
        std::string const& payload,
        std::string const& client_id,
        std::shared_ptr<Job>& out_job,
        std::string& token,
        std::string& error);
    [[nodiscard]] std::shared_ptr<Job> find_job(std::string_view job_id, std::string_view token) const;
    void reload_values();
    [[nodiscard]] tr_variant::Map make_values_object() const;
    [[nodiscard]] bool write_values() const;
    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> settings_get(tr_variant::Map& result) const;
    [[nodiscard]] std::pair<JsonRpc::Error::Code, std::string> settings_set(tr_variant::Map const& args, tr_variant::Map& result);

    std::string config_dir;
    Config config;
    mutable std::mutex jobs_mutex;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    mutable std::mutex values_mutex;
    bool values_available = true;
    std::string values_revision;
    std::map<std::string, std::map<std::string, std::shared_ptr<tr_variant>>> values;
    std::mutex restart_mutex;
    bool restart_scheduled = false;
    int64_t restart_scheduled_at = 0;
    int64_t last_restart_at = 0;
};

void log_config_error(std::string_view const message)
{
    tr_logAddError(fmt::format("script runner: {}", message));
}

void Impl::reload()
{
    auto lock = std::scoped_lock{ jobs_mutex };
    config = Config{};
    auto const path = fmt::format("{}/script-runner.json", config_dir);
    if (!tr_sys_path_exists(path))
    {
        config.enabled = false;
        return;
    }
    if (!load_config(path, config))
    {
        log_config_error("invalid script-runner.json");
        config.enabled = false;
        return;
    }
    if (config.restart.enabled && std::empty(config.restart.helper))
    {
        config.enabled = false;
        config.restart.enabled = false;
    }
    if (!config.enabled)
    {
        config.restart.enabled = false;
    }

    reload_values();
}

tr_variant::Map Impl::make_values_object() const
{
    auto scripts = tr_variant::Map{};
    for (auto const& [script_id, parameters] : values)
    {
        auto parameter_map = tr_variant::Map{};
        for (auto const& [key, value] : parameters)
        {
            parameter_map.try_emplace(q(key), value->clone());
        }
        scripts.try_emplace(q(script_id), tr_variant{ std::move(parameter_map) });
    }

    auto result = tr_variant::Map{ 2U };
    result.try_emplace(q("schema"), ScriptValuesSchema);
    result.try_emplace(q("scripts"), tr_variant{ std::move(scripts) });
    return result;
}

void Impl::reload_values()
{
    auto lock = std::scoped_lock{ values_mutex };
    values.clear();
    values_available = true;

    auto const path = fmt::format("{}/script-runner-values.json", config_dir);
    if (tr_sys_path_exists(path))
    {
        auto const var = tr_variant_serde::json().parse_file(path);
        auto const* const root = var ? var->get_if<tr_variant::Map>() : nullptr;
        auto const* const scripts = root != nullptr ? root->find_if<tr_variant::Map>(q("scripts")) : nullptr;
        if (root == nullptr || scripts == nullptr || get_string(*root, "schema") != ScriptValuesSchema)
        {
            values_available = false;
            log_config_error("invalid script-runner-values.json");
        }
        else
        {
            auto all_hosts = std::map<std::string, ExternalHost const*>{};
            for (auto const& host : config.external_hosts)
            {
                if (host.enabled)
                {
                    all_hosts.emplace(host.id, &host);
                }
            }

            for (auto const& [script_key, script_values] : *scripts)
            {
                auto const script_id = std::string{ tr_quark_get_string_view(script_key) };
                auto const script = std::ranges::find_if(
                    config.scripts,
                    [&script_id](auto const& item) { return item.id == script_id; });
                auto const* const parameter_map = script_values.get_if<tr_variant::Map>();
                if (script == std::end(config.scripts) || parameter_map == nullptr)
                {
                    continue;
                }

                auto stored = std::map<std::string, std::shared_ptr<tr_variant>>{};
                for (auto const& [parameter_key, value] : *parameter_map)
                {
                    auto const key = std::string{ tr_quark_get_string_view(parameter_key) };
                    auto const param = std::ranges::find_if(
                        script->parameters,
                        [&key](auto const& item) { return item.key == key && item.storage == "host"sv; });
                    if (param == std::end(script->parameters))
                    {
                        continue;
                    }
                    auto selected = std::map<std::string, ExternalHost const*>{};
                    auto error = std::string{};
                    if (validate_parameter(*param, value, selected, all_hosts, error))
                    {
                        stored.emplace(key, std::make_shared<tr_variant>(value.clone()));
                    }
                }
                if (!std::empty(stored))
                {
                    values.emplace(script_id, std::move(stored));
                }
            }
        }
    }

    auto const json = tr_variant_serde::json().compact().to_string(tr_variant{ make_values_object() });
    values_revision = fmt::format("sha256:{}", sha256_hex(json));
}

bool Impl::write_values() const
{
    auto const path = fmt::format("{}/script-runner-values.json", config_dir);
    auto const temp_path = fmt::format("{}.tmp", path);
    if (!tr_variant_serde::json().to_file(tr_variant{ make_values_object() }, temp_path))
    {
        log_config_error("couldn't write script-runner-values.json");
        return false;
    }

#ifdef _WIN32
    auto const moved = MoveFileExW(
        tr_win32_utf8_to_native(temp_path).c_str(),
        tr_win32_utf8_to_native(path).c_str(),
        MOVEFILE_REPLACE_EXISTING);
    if (moved == 0)
    {
        log_config_error("couldn't replace script-runner-values.json");
        return false;
    }
#else
    auto error = std::error_code{};
    std::filesystem::rename(temp_path, path, error);
    if (error)
    {
        log_config_error("couldn't replace script-runner-values.json");
        return false;
    }
#endif
    return true;
}

std::pair<JsonRpc::Error::Code, std::string> Impl::settings_get(tr_variant::Map& result) const
{
    using namespace JsonRpc;

    auto lock = std::scoped_lock{ values_mutex };
    if (!values_available)
    {
        return { Error::INVALID_PARAMS, "script settings unavailable"s };
    }

    auto scripts = tr_variant::Vector{};
    for (auto const& script : config.scripts)
    {
        if (!script.enabled || !is_script_available(script))
        {
            continue;
        }

        auto has_stored = false;
        auto parameter_map = tr_variant::Map{};
        auto missing = tr_variant::Vector{};
        for (auto const& param : script.parameters)
        {
            if (param.storage != "host"sv)
            {
                continue;
            }
            has_stored = true;
            auto const script_values = values.find(script.id);
            auto const* stored = static_cast<std::shared_ptr<tr_variant> const*>(nullptr);
            if (script_values != std::end(values))
            {
                auto const iter = script_values->second.find(param.key);
                if (iter != std::end(script_values->second))
                {
                    stored = &iter->second;
                }
            }
            if (stored != nullptr)
            {
                parameter_map.try_emplace(q(param.key), (*stored)->clone());
            }
            else if (param.default_value)
            {
                parameter_map.try_emplace(q(param.key), param.default_value->clone());
            }
            else if (param.required)
            {
                missing.emplace_back(param.key);
            }
        }
        if (!has_stored)
        {
            continue;
        }

        auto script_map = tr_variant::Map{ 4U };
        script_map.try_emplace(q("id"), script.id);
        script_map.try_emplace(q("parameters"), tr_variant{ std::move(parameter_map) });
        script_map.try_emplace(q("ready"), std::empty(missing));
        script_map.try_emplace(q("missing"), tr_variant{ std::move(missing) });
        scripts.emplace_back(std::move(script_map));
    }

    result.try_emplace(q("schema"), ScriptValuesSchema);
    result.try_emplace(q("revision"), values_revision);
    result.try_emplace(q("scripts"), tr_variant{ std::move(scripts) });
    return { Error::SUCCESS, std::string{} };
}

std::pair<JsonRpc::Error::Code, std::string> Impl::settings_set(tr_variant::Map const& args, tr_variant::Map& result)
{
    using namespace JsonRpc;

    auto lock = std::unique_lock{ values_mutex };
    if (!values_available)
    {
        return { Error::INVALID_PARAMS, "script settings unavailable"s };
    }

    if (auto const expected = get_string(args, "expected_revision"); expected && *expected != values_revision)
    {
        return { Error::INVALID_PARAMS, "script settings revision conflict"s };
    }

    auto const* const scripts = args.find_if<tr_variant::Vector>(q("scripts"));
    if (scripts == nullptr)
    {
        return { Error::INVALID_PARAMS, "invalid script parameters"s };
    }

    auto all_hosts = std::map<std::string, ExternalHost const*>{};
    for (auto const& host : config.external_hosts)
    {
        if (host.enabled)
        {
            all_hosts.emplace(host.id, &host);
        }
    }

    auto updated = values;
    for (auto const& script_var : *scripts)
    {
        auto const* const script_map = script_var.get_if<tr_variant::Map>();
        auto const script_id = script_map != nullptr ? get_string(*script_map, "id") : std::nullopt;
        auto const* const parameter_map = script_map != nullptr ? script_map->find_if<tr_variant::Map>(q("parameters")) : nullptr;
        if (!script_id || parameter_map == nullptr)
        {
            return { Error::INVALID_PARAMS, "invalid script parameters"s };
        }

        auto const script = std::ranges::find_if(
            config.scripts,
            [&script_id](auto const& item) { return item.id == *script_id && item.enabled && is_script_available(item); });
        if (script == std::end(config.scripts))
        {
            return { Error::INVALID_PARAMS, "invalid script parameters"s };
        }

        auto has_stored = false;
        for (auto const& param : script->parameters)
        {
            has_stored = has_stored || param.storage == "host"sv;
        }
        if (!has_stored)
        {
            return { Error::INVALID_PARAMS, "invalid script parameters"s };
        }

        auto stored = std::map<std::string, std::shared_ptr<tr_variant>>{};
        for (auto const& [key_quark, value] : *parameter_map)
        {
            auto const key = std::string{ tr_quark_get_string_view(key_quark) };
            auto const param = std::ranges::find_if(
                script->parameters,
                [&key](auto const& item) { return item.key == key && item.storage == "host"sv; });
            if (param == std::end(script->parameters))
            {
                return { Error::INVALID_PARAMS, "invalid script parameters"s };
            }
            auto selected = std::map<std::string, ExternalHost const*>{};
            auto error = std::string{};
            if (!validate_parameter(*param, value, selected, all_hosts, error))
            {
                return { Error::INVALID_PARAMS, "invalid script parameters"s };
            }
            stored.emplace(key, std::make_shared<tr_variant>(value.clone()));
        }

        for (auto const& param : script->parameters)
        {
            if (param.storage == "host"sv && param.required && stored.find(param.key) == std::end(stored) && !param.default_value)
            {
                return { Error::INVALID_PARAMS, "script settings incomplete"s };
            }
        }

        updated.insert_or_assign(std::string{ *script_id }, std::move(stored));
    }

    auto const old_values = values;
    auto const old_revision = values_revision;
    values = std::move(updated);
    auto const json = tr_variant_serde::json().compact().to_string(tr_variant{ make_values_object() });
    values_revision = fmt::format("sha256:{}", sha256_hex(json));
    if (!write_values())
    {
        values = old_values;
        values_revision = old_revision;
        return { Error::SYSTEM_ERROR, "script settings unavailable"s };
    }

    lock.unlock();
    return settings_get(result);
}
void Impl::cancel_all()
{
    auto jobs_copy = std::vector<std::shared_ptr<Job>>{};
    {
        auto lock = std::scoped_lock{ jobs_mutex };
        for (auto const& [id, job] : jobs)
        {
            jobs_copy.emplace_back(job);
        }
    }
#ifdef _WIN32
    for (auto const& job : jobs_copy)
    {
        if (auto process = job->process; process != nullptr)
        {
            process->cancel_requested = true;
            if (process->job != nullptr)
            {
                TerminateJobObject(process->job, 1U);
            }
        }
    }
#else
    for (auto const& job : jobs_copy)
    {
        if (auto process = job->process; process != nullptr)
        {
            process->cancel_requested = true;
            if (process->pid > 0)
            {
                kill(-process->pid, SIGTERM);
            }
        }
    }
#endif
}

[[nodiscard]] std::shared_ptr<Job> Impl::find_job(std::string_view const job_id, std::string_view const token) const
{
    auto lock = std::scoped_lock{ jobs_mutex };
    auto const iter = jobs.find(std::string{ job_id });
    if (iter == std::end(jobs) || iter->second->token_hash != sha256_hex(token))
    {
        return {};
    }
    return iter->second;
}

bool Impl::start_job(
    tr_session* session,
    ScriptDefinition const& script,
    tr_variant::Map const& /*parameters*/,
    tr_variant::Map const& /*external_hosts*/,
    std::string const& payload,
    std::string const& client_id,
    std::shared_ptr<Job>& out_job,
    std::string& token,
    std::string& error)
{
    auto job = std::make_shared<Job>();
    job->id = random_uuid();
    token = random_hex(32);
    job->token_hash = sha256_hex(token);
    job->script_id = script.id;
    job->max_output = script.max_output_bytes;
    job->queued_at = static_cast<int64_t>(std::time(nullptr));
    job->started_at = job->queued_at;
    tr_logAddInfo(fmt::format(
        "script runner: starting job {} script '{}' client '{}'",
        job->id,
        script.id,
        client_id));

    auto env = script.env;
    auto const script_rpc_url = rpc_url(*session);
    if (!std::empty(script_rpc_url))
    {
        env.insert_or_assign("TR_SCRIPT_RPC_URL", script_rpc_url);
    }
    if (!std::empty(config.rpc_netrc_file))
    {
        env.insert_or_assign("TR_SCRIPT_RPC_NETRC", config.rpc_netrc_file);
    }

#ifdef _WIN32
    auto process = std::make_shared<ProcessRuntime>();
    job->process = process;

    auto sa = SECURITY_ATTRIBUTES{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (CreatePipe(&process->stdin_read, &process->stdin_write, &sa, 0) == 0 ||
        CreatePipe(&process->stdout_read, &process->stdout_write, &sa, 0) == 0 ||
        CreatePipe(&process->stderr_read, &process->stderr_write, &sa, 0) == 0)
    {
        error = "script execution failed";
        return false;
    }

    SetHandleInformation(process->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(process->stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(process->stderr_read, HANDLE_FLAG_INHERIT, 0);

    auto cmdline = quote_argument(script.executable);
    for (auto const& arg : script.args)
    {
        cmdline.push_back(L' ');
        cmdline += quote_argument(arg);
    }

    auto env_block = std::vector<wchar_t>{};
    append_environment(env, env_block);

    auto si = STARTUPINFOEXW{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = process->stdin_read;
    si.StartupInfo.hStdOutput = process->stdout_write;
    si.StartupInfo.hStdError = process->stderr_write;

    auto child_handles = std::array<HANDLE, 3U>{
        process->stdin_read,
        process->stdout_write,
        process->stderr_write,
    };
    auto attr_size = SIZE_T{};
    InitializeProcThreadAttributeList(nullptr, 1U, 0, &attr_size);
    auto attr_buf = std::vector<BYTE>(attr_size);
    auto* const attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(std::data(attr_buf));
    auto const attrs_ok = InitializeProcThreadAttributeList(attrs, 1U, 0, &attr_size) != 0;
    if (attrs_ok)
    {
        UpdateProcThreadAttribute(
            attrs,
            0U,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            std::data(child_handles),
            sizeof(child_handles),
            nullptr,
            nullptr);
        si.lpAttributeList = attrs;
    }

    auto pi = PROCESS_INFORMATION{};
    auto const created = CreateProcessW(
        nullptr,
        std::data(cmdline),
        nullptr,
        nullptr,
        TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        std::data(env_block),
        tr_win32_utf8_to_native(script.cwd).c_str(),
        &si.StartupInfo,
        &pi);
    if (attrs_ok)
    {
        DeleteProcThreadAttributeList(attrs);
    }
    if (created == 0)
    {
        error = fmt::format("script execution failed: CreateProcessW error {}", GetLastError());
        return false;
    }

    process->process = pi.hProcess;
    process->job = CreateJobObjectW(nullptr, nullptr);
    if (process->job != nullptr)
    {
        auto info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(process->job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(process->job, process->process);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    CloseHandle(process->stdin_read);
    process->stdin_read = nullptr;
    CloseHandle(process->stdout_write);
    process->stdout_write = nullptr;
    CloseHandle(process->stderr_write);
    process->stderr_write = nullptr;

    {
        auto lock = std::scoped_lock{ jobs_mutex };
        jobs.emplace(job->id, job);
    }

    {
        auto lock = std::scoped_lock{ job->mutex };
        job->state = Job::State::Running;
        job->started_at = static_cast<int64_t>(std::time(nullptr));
    }

    auto* const job_ptr = job.get();
    auto* const process_ptr = process.get();
    process->stdout_thread = std::thread([process_ptr, job_ptr] { read_pipe(process_ptr->stdout_read, job_ptr, true); });
    process->stderr_thread = std::thread([process_ptr, job_ptr] { read_pipe(process_ptr->stderr_read, job_ptr, false); });
    process->stdin_thread = std::thread(
        [process_ptr, payload]()
        {
            auto written = DWORD{};
            WriteFile(
                process_ptr->stdin_write,
                std::data(payload),
                static_cast<DWORD>(std::size(payload)),
                &written,
                nullptr);
            CloseHandle(process_ptr->stdin_write);
            process_ptr->stdin_write = nullptr;
        });
    process->waiter = std::thread(
        [process_ptr, job_ptr, timeout_seconds = script.timeout_seconds, client_id]()
        {
            auto const timeout_ms = static_cast<DWORD>(
                std::min<int64_t>(timeout_seconds * 1000, static_cast<int64_t>(INFINITE) - 1));
            auto const wait_result = WaitForSingleObject(process_ptr->process, timeout_ms);
            if (wait_result == WAIT_TIMEOUT)
            {
                process_ptr->timeout_requested = true;
                if (process_ptr->job != nullptr)
                {
                    TerminateJobObject(process_ptr->job, 1U);
                }
                WaitForSingleObject(process_ptr->process, 5000);
            }
            if (process_ptr->cancel_requested)
            {
                if (process_ptr->job != nullptr)
                {
                    TerminateJobObject(process_ptr->job, 1U);
                }
                WaitForSingleObject(process_ptr->process, 5000);
            }

            auto exit_code = DWORD{};
            GetExitCodeProcess(process_ptr->process, &exit_code);

            if (process_ptr->stdin_thread.joinable())
            {
                process_ptr->stdin_thread.join();
            }
            if (process_ptr->stdout_thread.joinable())
            {
                process_ptr->stdout_thread.join();
            }
            if (process_ptr->stderr_thread.joinable())
            {
                process_ptr->stderr_thread.join();
            }

            {
                auto lock = std::scoped_lock{ job_ptr->mutex };
                job_ptr->exit_code = static_cast<int64_t>(exit_code);
                job_ptr->has_exit_code = true;
                job_ptr->finished_at = static_cast<int64_t>(std::time(nullptr));
                if (process_ptr->cancel_requested)
                {
                    job_ptr->state = Job::State::Canceled;
                    job_ptr->canceled = true;
                }
                else if (process_ptr->timeout_requested)
                {
                    job_ptr->state = Job::State::TimedOut;
                    job_ptr->timed_out = true;
                }
                else if (exit_code == 0)
                {
                    job_ptr->state = Job::State::Finished;
                }
                else
                {
                    job_ptr->state = Job::State::Failed;
                }
            }

            CloseHandle(process_ptr->stdout_read);
            process_ptr->stdout_read = nullptr;
            CloseHandle(process_ptr->stderr_read);
            process_ptr->stderr_read = nullptr;
            CloseHandle(process_ptr->process);
            process_ptr->process = nullptr;
            CloseHandle(process_ptr->job);
            process_ptr->job = nullptr;
        });

    out_job = job;
    return true;
#else
    auto process = std::make_shared<ProcessRuntime>();
    job->process = process;

    auto stdin_pipe = std::array<int, 2>{ -1, -1 };
    auto stdout_pipe = std::array<int, 2>{ -1, -1 };
    auto stderr_pipe = std::array<int, 2>{ -1, -1 };
    if (pipe(std::data(stdin_pipe)) != 0 || pipe(std::data(stdout_pipe)) != 0 || pipe(std::data(stderr_pipe)) != 0)
    {
        error = "script execution failed";
        return false;
    }

    auto const max_fd = [] {
        auto const limit = sysconf(_SC_OPEN_MAX);
        return static_cast<int>(limit > 0 ? std::min<long>(limit, 65536) : 1024);
    }();

    auto argv_storage = std::vector<std::string>{};
    argv_storage.reserve(1U + std::size(script.args));
    argv_storage.emplace_back(script.executable);
    argv_storage.insert(std::end(argv_storage), std::begin(script.args), std::end(script.args));
    auto argv = std::vector<char*>{};
    argv.reserve(std::size(argv_storage) + 1U);
    for (auto& arg : argv_storage)
    {
        argv.emplace_back(arg.data());
    }
    argv.emplace_back(nullptr);

    auto environment = std::map<std::string, std::string>{};
    for (auto const* const name : { "PATH", "HOME", "LANG", "LC_ALL", "TMPDIR" })
    {
        if (auto const* const value = std::getenv(name); value != nullptr)
        {
            environment.emplace(name, value);
        }
    }
    for (auto const& [key, value] : env)
    {
        environment.insert_or_assign(key, value);
    }
    auto env_storage = std::vector<std::string>{};
    env_storage.reserve(std::size(environment));
    for (auto const& [key, value] : environment)
    {
        env_storage.emplace_back(fmt::format("{}={}", key, value));
    }
    auto envp = std::vector<char*>{};
    envp.reserve(std::size(env_storage) + 1U);
    for (auto& item : env_storage)
    {
        envp.emplace_back(item.data());
    }
    envp.emplace_back(nullptr);

    auto const pid = fork();
    if (pid < 0)
    {
        error = "script execution failed: fork";
        return false;
    }

    if (pid == 0)
    {
        setpgid(0, 0);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        for (int fd = 3; fd < max_fd; ++fd)
        {
            close(fd);
        }

        struct sigaction action = {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPIPE, &action, nullptr);
        auto mask = sigset_t{};
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, nullptr);

        if (chdir(script.cwd.c_str()) != 0)
        {
            _exit(127);
        }
        execve(argv[0], std::data(argv), std::data(envp));
        _exit(127);
    }

    setpgid(pid, pid);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    process->pid = pid;
    process->stdin_write = stdin_pipe[1];
    process->stdout_read = stdout_pipe[0];
    process->stderr_read = stderr_pipe[0];

    {
        auto lock = std::scoped_lock{ jobs_mutex };
        jobs.emplace(job->id, job);
    }

    {
        auto lock = std::scoped_lock{ job->mutex };
        job->state = Job::State::Running;
        job->started_at = static_cast<int64_t>(std::time(nullptr));
    }

    auto* const job_ptr = job.get();
    auto* const process_ptr = process.get();
    process->stdout_thread = std::thread([process_ptr, job_ptr] { read_pipe(process_ptr->stdout_read, job_ptr, true); });
    process->stderr_thread = std::thread([process_ptr, job_ptr] { read_pipe(process_ptr->stderr_read, job_ptr, false); });
    process->stdin_thread = std::thread(
        [process_ptr, payload]()
        {
            auto const* data = std::data(payload);
            auto remaining = std::size(payload);
            while (remaining > 0)
            {
                auto const written = write(process_ptr->stdin_write, data, remaining);
                if (written < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    break;
                }
                data += written;
                remaining -= static_cast<size_t>(written);
            }
            close_fd(process_ptr->stdin_write);
        });
    process->waiter = std::thread(
        [process_ptr, job_ptr, timeout_seconds = script.timeout_seconds, client_id]()
        {
            auto status = int{};
            auto term_sent = false;
            auto kill_sent = false;
            auto const started = std::chrono::steady_clock::now();
            auto const deadline = started + std::chrono::seconds(timeout_seconds);
            auto grace_deadline = started;

            for (;;)
            {
                auto const result = waitpid(process_ptr->pid, &status, WNOHANG);
                if (result == process_ptr->pid)
                {
                    break;
                }
                if (result < 0 && errno == ECHILD)
                {
                    break;
                }

                auto const now = std::chrono::steady_clock::now();
                if (!process_ptr->timeout_requested && now >= deadline)
                {
                    process_ptr->timeout_requested = true;
                }

                if (process_ptr->cancel_requested || process_ptr->timeout_requested)
                {
                    if (!term_sent)
                    {
                        kill(-process_ptr->pid, SIGTERM);
                        term_sent = true;
                        grace_deadline = now + std::chrono::seconds(5);
                    }
                    else if (!kill_sent && now >= grace_deadline)
                    {
                        kill(-process_ptr->pid, SIGKILL);
                        kill_sent = true;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (term_sent)
            {
                kill(-process_ptr->pid, SIGKILL);
            }

            if (process_ptr->stdin_thread.joinable())
            {
                process_ptr->stdin_thread.join();
            }
            if (process_ptr->stdout_thread.joinable())
            {
                process_ptr->stdout_thread.join();
            }
            if (process_ptr->stderr_thread.joinable())
            {
                process_ptr->stderr_thread.join();
            }

            {
                auto lock = std::scoped_lock{ job_ptr->mutex };
                job_ptr->finished_at = static_cast<int64_t>(std::time(nullptr));
                if (WIFEXITED(status))
                {
                    job_ptr->exit_code = static_cast<int64_t>(WEXITSTATUS(status));
                    job_ptr->has_exit_code = true;
                }
                else if (WIFSIGNALED(status))
                {
                    job_ptr->term_signal = static_cast<int64_t>(WTERMSIG(status));
                    job_ptr->has_term_signal = true;
                }

                if (process_ptr->cancel_requested)
                {
                    job_ptr->state = Job::State::Canceled;
                    job_ptr->canceled = true;
                }
                else if (process_ptr->timeout_requested)
                {
                    job_ptr->state = Job::State::TimedOut;
                    job_ptr->timed_out = true;
                }
                else if (job_ptr->has_exit_code && job_ptr->exit_code == 0)
                {
                    job_ptr->state = Job::State::Finished;
                }
                else
                {
                    job_ptr->state = Job::State::Failed;
                }
            }

            close_fd(process_ptr->stdout_read);
            close_fd(process_ptr->stderr_read);
            close_fd(process_ptr->stdin_write);
            process_ptr->pid = -1;
        });

    out_job = job;
    return true;
#endif
}

} // namespace

struct tr_script_runner::Impl final : ::Impl
{
    using ::Impl::Impl;
};

namespace
{

[[nodiscard]] std::string_view state_string(Job::State const state)
{
    switch (state)
    {
    case Job::State::Queued:
        return "queued"sv;
    case Job::State::Running:
        return "running"sv;
    case Job::State::Finished:
        return "finished"sv;
    case Job::State::Failed:
        return "failed"sv;
    case Job::State::Canceled:
        return "canceled"sv;
    case Job::State::TimedOut:
        return "timed_out"sv;
    }

    return "failed"sv;
}

[[nodiscard]] std::string_view output_slice(std::string const& input, int64_t const offset, int64_t const limit)
{
    if (offset < 0 || limit <= 0 || offset >= static_cast<int64_t>(std::size(input)))
    {
        return {};
    }
    return std::string_view{ input }.substr(static_cast<size_t>(offset), static_cast<size_t>(limit));
}

} // namespace

tr_script_runner::tr_script_runner(std::string_view const config_dir)
    : impl_{ std::make_unique<tr_script_runner::Impl>(config_dir) }
{
}

tr_script_runner::~tr_script_runner() = default;

void tr_script_runner::reload()
{
    impl_->reload();
}

bool tr_script_runner::is_enabled() const
{
    return impl_->config.enabled;
}

bool tr_script_runner::is_restart_enabled() const
{
    return impl_->config.enabled && impl_->config.restart.enabled;
}

tr_variant tr_script_runner::get_descriptor(tr_session const& /*session*/) const
{
    auto const& config = impl_->config;

    auto external_hosts = tr_variant::Vector{};
    auto hosts = config.external_hosts;
    std::ranges::sort(hosts, [](auto const& lhs, auto const& rhs) { return lhs.id < rhs.id; });
    for (auto const& host : hosts)
    {
        if (host.enabled)
        {
            external_hosts.emplace_back(make_external_host_descriptor(host));
        }
    }

    auto scripts = tr_variant::Vector{};
    auto definitions = std::vector<ScriptDefinition const*>{};
    definitions.reserve(std::size(config.scripts));
    for (auto const& script : config.scripts)
    {
        definitions.emplace_back(&script);
    }
    std::ranges::sort(definitions, [](auto const* lhs, auto const* rhs) { return lhs->id < rhs->id; });
    for (auto const* const script : definitions)
    {
        if (script->enabled && is_script_available(*script))
        {
            scripts.emplace_back(make_script_descriptor(*script));
        }
    }

    auto restart_map = tr_variant::Map{ 4U };
    restart_map.try_emplace(q("id"), "restart-daemon"sv);
    restart_map.try_emplace(q("name"), "Restart daemon"sv);
    restart_map.try_emplace(q("context"), "system"sv);
    restart_map.try_emplace(q("enabled"), config.restart.enabled);

    auto revision_input = tr_variant::Map{ 4U };
    revision_input.try_emplace(q("schema"), ScriptRunnerSchema);
    revision_input.try_emplace(q("external_hosts"), tr_variant{ clone_vector(external_hosts) });
    auto revision_restart = tr_variant::Map{ 1U };
    revision_restart.try_emplace(q("enabled"), config.restart.enabled);
    revision_input.try_emplace(q("restart_daemon"), tr_variant{ std::move(revision_restart) });
    revision_input.try_emplace(q("scripts"), tr_variant{ clone_vector(scripts) });

    auto const revision_json = tr_variant_serde::json().compact().to_string(tr_variant{ std::move(revision_input) });
    auto const revision = fmt::format("sha256:{}", sha256_hex(revision_json));

    auto descriptor = tr_variant::Map{};
    descriptor.try_emplace(q("schema"), ScriptRunnerSchema);
    descriptor.try_emplace(q("revision"), revision);
    descriptor.try_emplace(q("external_hosts"), tr_variant{ std::move(external_hosts) });
    descriptor.try_emplace(q("restart_daemon"), tr_variant{ std::move(restart_map) });
    descriptor.try_emplace(q("scripts"), tr_variant{ std::move(scripts) });
    return tr_variant{ std::move(descriptor) };
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::run(
    tr_session* session,
    tr_variant::Map const& args,
    tr_variant::Map& result)
{
    using namespace JsonRpc;

    if (!is_enabled())
    {
        return { Error::INVALID_PARAMS, "script runner is disabled"s };
    }

    auto const id = get_string(args, "id");
    if (!id)
    {
        return { Error::INVALID_PARAMS, "invalid script payload"s };
    }

    auto const& config = impl_->config;
    auto const script_iter = std::ranges::find_if(
        config.scripts,
        [&id](auto const& item) { return item.id == *id; });
    if (script_iter == std::end(config.scripts))
    {
        return { Error::INVALID_PARAMS, "script not found"s };
    }
    if (!script_iter->enabled)
    {
        return { Error::INVALID_PARAMS, "script is disabled"s };
    }
    if (!is_script_available(*script_iter))
    {
        return { Error::INVALID_PARAMS, "script is not available on this platform"s };
    }

    auto const* const payload = args.find_if<tr_variant::Map>(q("payload"));
    if (payload == nullptr)
    {
        return { Error::INVALID_PARAMS, "invalid script payload"s };
    }
    auto const context = payload->value_if<std::string_view>(q("context"));
    if (!context || !valid_context(*context))
    {
        return { Error::INVALID_PARAMS, "invalid script payload"s };
    }
    if (std::ranges::find(script_iter->contexts, *context) == std::end(script_iter->contexts))
    {
        return { Error::INVALID_PARAMS, "script does not support this context"s };
    }

    auto const* const parameters_in = payload->find_if<tr_variant::Map>(q("parameters"));
    auto parameters = tr_variant::Map{};
    auto selected_hosts = std::map<std::string, ExternalHost const*>{};
    auto error = std::string{};
    {
        auto lock = std::scoped_lock{ impl_->values_mutex };
        auto const stored_iter = impl_->values.find(script_iter->id);
        auto const* const stored = stored_iter != std::end(impl_->values) ? &stored_iter->second : nullptr;
        if (!resolve_parameters(
                config,
                *script_iter,
                parameters_in,
                stored,
                impl_->values_available,
                parameters,
                selected_hosts,
                error))
        {
            return { Error::INVALID_PARAMS, std::move(error) };
        }
    }

    auto payload_text = std::string{};
    if (!build_execution_payload(session, config, *script_iter, parameters, selected_hosts, *payload, payload_text, error))
    {
        return { Error::INVALID_PARAMS, std::move(error) };
    }

    auto job = std::shared_ptr<Job>{};
    auto token = std::string{};
    auto const client_id = get_string(args, "client_id").value_or(""sv);
    if (!impl_->start_job(
            session,
            *script_iter,
            parameters,
            tr_variant::Map{},
            payload_text,
            std::string{ client_id },
            job,
            token,
            error))
    {
        tr_logAddError(fmt::format(
            "script runner: failed to start script '{}' client '{}': {}",
            script_iter->id,
            client_id,
            error));
        return { Error::SYSTEM_ERROR, std::move(error) };
    }

    result.try_emplace(q("job_id"), job->id);
    result.try_emplace(q("job_token"), token);
    result.try_emplace(q("script_id"), job->script_id);
    result.try_emplace(q("state"), "queued"sv);
    result.try_emplace(q("deduplicated"), false);
    return { Error::SUCCESS, std::string{} };
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::status(
    tr_variant::Map const& args,
    tr_variant::Map& result)
{
    using namespace JsonRpc;

    auto const job_id = get_string(args, "job_id");
    auto const token = get_string(args, "job_token");
    if (!job_id || !token)
    {
        return { Error::INVALID_PARAMS, "job not found"s };
    }

    auto const job = impl_->find_job(*job_id, *token);
    if (job == nullptr)
    {
        return { Error::INVALID_PARAMS, "job not found"s };
    }

    auto const stdout_offset = get_int(args, "stdout_offset").value_or(0);
    auto const stderr_offset = get_int(args, "stderr_offset").value_or(0);
    auto const stdout_limit = get_int(args, "stdout_limit").value_or(65536);
    auto const stderr_limit = get_int(args, "stderr_limit").value_or(65536);
    if (stdout_offset < 0 || stderr_offset < 0 || stdout_limit < 0 || stderr_limit < 0 || stdout_limit > 1048576 ||
        stderr_limit > 1048576)
    {
        return { Error::INVALID_PARAMS, "invalid output range"s };
    }

    auto lock = std::scoped_lock{ job->mutex };

    result.try_emplace(q("job_id"), job->id);
    result.try_emplace(q("script_id"), job->script_id);
    result.try_emplace(q("state"), state_string(job->state));
    if (job->has_exit_code)
    {
        result.try_emplace(q("exit_code"), job->exit_code);
    }
    else
    {
        result.try_emplace(q("exit_code"), tr_variant{ std::nullptr_t{} });
    }
    if (job->has_term_signal)
    {
        result.try_emplace(q("term_signal"), job->term_signal);
    }
    else
    {
        result.try_emplace(q("term_signal"), tr_variant{ std::nullptr_t{} });
    }
    if (std::empty(job->error))
    {
        result.try_emplace(q("error"), tr_variant{ std::nullptr_t{} });
    }
    else
    {
        result.try_emplace(q("error"), job->error);
    }

    auto const stdout_slice = output_slice(job->stdout_buf, stdout_offset, stdout_limit);
    auto const stderr_slice = output_slice(job->stderr_buf, stderr_offset, stderr_limit);
    result.try_emplace(q("stdout"), stdout_slice);
    result.try_emplace(q("stderr"), stderr_slice);
    result.try_emplace(q("stdout_offset"), stdout_offset);
    result.try_emplace(q("stdout_total_bytes"), job->stdout_total);
    result.try_emplace(q("stdout_stored_bytes"), job->stdout_stored);
    result.try_emplace(q("stdout_truncated"), job->stdout_truncated);
    result.try_emplace(q("stderr_offset"), stderr_offset);
    result.try_emplace(q("stderr_total_bytes"), job->stderr_total);
    result.try_emplace(q("stderr_stored_bytes"), job->stderr_stored);
    result.try_emplace(q("stderr_truncated"), job->stderr_truncated);
    result.try_emplace(q("truncated"), job->stdout_truncated || job->stderr_truncated);
    result.try_emplace(q("timed_out"), job->timed_out);
    result.try_emplace(q("queued_at"), job->queued_at);
    if (job->state == Job::State::Queued)
    {
        result.try_emplace(q("started_at"), tr_variant{ std::nullptr_t{} });
    }
    else
    {
        result.try_emplace(q("started_at"), job->started_at);
    }
    if (job->finished_at == 0)
    {
        result.try_emplace(q("finished_at"), tr_variant{ std::nullptr_t{} });
    }
    else
    {
        result.try_emplace(q("finished_at"), job->finished_at);
    }

    return { Error::SUCCESS, std::string{} };
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::cancel(
    tr_variant::Map const& args,
    tr_variant::Map& result)
{
    using namespace JsonRpc;

    auto const job_id = get_string(args, "job_id");
    auto const token = get_string(args, "job_token");
    if (!job_id || !token)
    {
        return { Error::INVALID_PARAMS, "job not found"s };
    }

    auto const job = impl_->find_job(*job_id, *token);
    if (job == nullptr)
    {
        return { Error::INVALID_PARAMS, "job not found"s };
    }

    auto canceled = false;
    {
        auto lock = std::scoped_lock{ job->mutex };
        if (job->state == Job::State::Queued || job->state == Job::State::Running)
        {
            job->canceled = true;
            if (job->state == Job::State::Queued)
            {
                job->state = Job::State::Canceled;
            }
            canceled = true;
        }
    }
#ifdef _WIN32
    if (auto process = job->process; process != nullptr)
    {
        process->cancel_requested = true;
        if (process->job != nullptr)
        {
            TerminateJobObject(process->job, 1U);
        }
    }
#else
    if (auto process = job->process; process != nullptr)
    {
        process->cancel_requested = true;
        if (process->pid > 0)
        {
            kill(-process->pid, SIGTERM);
        }
    }
#endif

    auto lock = std::scoped_lock{ job->mutex };
    result.try_emplace(q("job_id"), job->id);
    result.try_emplace(q("canceled"), canceled);
    result.try_emplace(q("state"), state_string(job->state));
    return { Error::SUCCESS, std::string{} };
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::restart_daemon(
    tr_variant::Map const& args,
    tr_variant::Map& result)
{
    using namespace JsonRpc;

    if (!is_restart_enabled())
    {
        return { Error::INVALID_PARAMS, "restart is disabled"s };
    }

    auto lock = std::scoped_lock{ impl_->restart_mutex };
    auto const now = static_cast<int64_t>(std::time(nullptr));
    if (impl_->restart_scheduled)
    {
        return { Error::INVALID_PARAMS, "restart already scheduled"s };
    }
    if (impl_->last_restart_at != 0 && now - impl_->last_restart_at < impl_->config.restart.cooldown_seconds)
    {
        return { Error::INVALID_PARAMS, "restart cooldown active"s };
    }

    auto const restart_id = random_uuid();
    auto const scheduled_at = now + impl_->config.restart.delay_seconds;
    impl_->restart_scheduled = true;
    impl_->restart_scheduled_at = scheduled_at;
    impl_->last_restart_at = now;

    auto const helper = impl_->config.restart.helper;
    auto const cwd = impl_->config.restart.cwd;
    auto const delay = impl_->config.restart.delay_seconds;
    auto const args_copy = impl_->config.restart.args;
    auto const client_id = get_string(args, "client_id").value_or(""sv);

    std::thread(
        [helper, cwd, delay, args_copy, client_id]()
        {
            if (delay > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            }
            auto cmd_storage = std::vector<std::string>{};
            cmd_storage.reserve(1U + std::size(args_copy));
            cmd_storage.emplace_back(helper);
            cmd_storage.insert(std::end(cmd_storage), std::begin(args_copy), std::end(args_copy));
            auto cmd = std::vector<char const*>{};
            cmd.reserve(std::size(cmd_storage) + 1U);
            for (auto const& item : cmd_storage)
            {
                cmd.emplace_back(item.c_str());
            }
            cmd.emplace_back(nullptr);
            auto env = std::map<std::string_view, std::string_view>{};
            auto error = tr_error{};
            if (!tr_spawn_async(std::data(cmd), env, cwd, &error))
            {
                tr_logAddError(fmt::format("script runner: restart helper failed for '{}': {}", client_id, error.message()));
            }
        })
        .detach();

    result.try_emplace(q("accepted"), true);
    result.try_emplace(q("restart_id"), restart_id);
    result.try_emplace(q("state"), "scheduled"sv);
    result.try_emplace(q("scheduled_at"), scheduled_at);
    return { Error::SUCCESS, std::string{} };
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::settings_get(tr_variant::Map& result)
{
    if (!is_enabled())
    {
        return { JsonRpc::Error::INVALID_PARAMS, "script runner is disabled"s };
    }
    return impl_->settings_get(result);
}

std::pair<JsonRpc::Error::Code, std::string> tr_script_runner::settings_set(
    tr_variant::Map const& args,
    tr_variant::Map& result)
{
    if (!is_enabled())
    {
        return { JsonRpc::Error::INVALID_PARAMS, "script runner is disabled"s };
    }
    return impl_->settings_set(args, result);
}