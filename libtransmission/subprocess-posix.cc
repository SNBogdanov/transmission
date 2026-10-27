// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "libtransmission/error.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"

using namespace std::literals;

namespace
{
void set_system_error(tr_error* error, int code, std::string_view what)
{
    if (error != nullptr)
    {
        error->set(code, fmt::format("{:s} failed: {:s} ({:d})", what, tr_strerror(code), code));
    }
}

[[nodiscard]] bool tr_spawn_async_in_child(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir)
{
    auto key_sz = std::string{};
    auto val_sz = std::string{};

    for (auto const& [key_sv, val_sv] : env)
    {
        key_sz = key_sv;
        val_sz = val_sv;

        if (setenv(key_sz.c_str(), val_sz.c_str(), 1) != 0)
        {
            return false;
        }
    }

    if (!std::empty(work_dir) && chdir(tr_pathbuf{ work_dir }) == -1)
    {
        return false;
    }

    if (execvp(cmd[0], const_cast<char* const*>(cmd)) == -1)
    {
        return false;
    }

    return true;
}

[[nodiscard]] bool tr_spawn_async_in_parent(int pipe_fd, tr_error* error)
{
    auto child_errno = int{};
    auto n_read = ssize_t{};
    for (auto done = false; !done;)
    {
        n_read = read(pipe_fd, &child_errno, sizeof(child_errno));
        done = n_read != -1 || errno != EINTR;
    }

    close(pipe_fd);

    if (n_read == 0) // child successfully exec'ed
    {
        return true;
    }

    if (n_read > 0) // child errno was set
    {
        TR_ASSERT(static_cast<size_t>(n_read) == sizeof(child_errno));
        set_system_error(error, child_errno, "Child process setup");
        return false;
    }

    // read failed (what to do?)
    return true;
}
} // namespace

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    tr_error* error,
    tr_spawn_callback on_exit)
{
    auto pipe_fds = std::array<int, 2>{};

    if (pipe(std::data(pipe_fds)) == -1)
    {
        set_system_error(error, errno, "Call to pipe()");
        return false;
    }

    if (fcntl(pipe_fds[1], F_SETFD, fcntl(pipe_fds[1], F_GETFD) | FD_CLOEXEC) == -1)
    {
        set_system_error(error, errno, "Call to fcntl()");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    int const child_pid = fork();

    if (child_pid == -1)
    {
        set_system_error(error, errno, "Call to fork()");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    if (child_pid == 0)
    {
        close(pipe_fds[0]);

        if (!tr_spawn_async_in_child(cmd, env, work_dir))
        {
            auto const ok = write(pipe_fds[1], &errno, sizeof(errno)) != -1;
            _exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    close(pipe_fds[1]);

    auto reaper = std::thread(
        [child_pid, on_exit = std::move(on_exit)]() mutable
        {
            auto status = int{};

            if (waitpid(child_pid, &status, 0) == child_pid && on_exit)
            {
                on_exit(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        });
    reaper.detach();

    return tr_spawn_async_in_parent(pipe_fds[0], error);
}
