// SPDX-License-Identifier: GPL-3.0-or-later
// Beast handles WebSockets; the process boundary uses JSONL in, length-framed JSON out.
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/beast.hpp>
#include <deque>
#include <iostream>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;
namespace asio = boost::asio;
namespace beast = boost::beast;
using Socket = asio::local::stream_protocol::socket;
constexpr std::size_t Limit = 16 * 1024 * 1024;

class Bridge {
    asio::io_context io;
    beast::websocket::stream<Socket> ws{io};
    asio::posix::stream_descriptor input{io, ::dup(STDIN_FILENO)}, output{io, ::dup(STDOUT_FILENO)};
    asio::signal_set signals{io, SIGINT, SIGTERM};
    asio::steady_timer deadline{io};
    asio::streambuf lines{Limit};
    beast::flat_buffer incoming{Limit};
    std::deque<std::string> writes;
    std::size_t queued = 0;
    std::string outgoing;
    pid_t child = -1;
    bool stopped = false;
    int result = 0;
    void stop(const char *message = nullptr) {
        if (stopped)
            return;
        stopped = true;
        if (message) {
            std::cerr << message << '\n';
            result = 1;
        }
        boost::system::error_code ec;
        ws.next_layer().close(ec);
        input.close(ec);
        output.close(ec);
        io.stop();
    }
    void readInput() {
        asio::async_read_until(input, lines, '\n', [this](auto ec, std::size_t size) {
            if (ec) {
                stop(ec == asio::error::eof ? nullptr
                                            : "Input stream failed or message exceeded 16 MiB.");
                return;
            }
            std::istream stream(&lines);
            std::string line;
            std::getline(stream, line);
            if (queued + size > Limit) {
                stop("Outgoing queue exceeded 16 MiB.");
                return;
            }
            if (!line.empty()) {
                queued += line.size();
                writes.push_back(std::move(line));
                if (writes.size() == 1)
                    writeNext();
            }
            readInput();
        });
    }
    void writeNext() {
        ws.async_write(asio::buffer(writes.front()), [this](auto ec, std::size_t) {
            if (ec) {
                stop("WebSocket write failed.");
                return;
            }
            queued -= writes.front().size();
            writes.pop_front();
            if (!writes.empty())
                writeNext();
        });
    }
    void readSocket() {
        ws.async_read(incoming, [this](auto ec, std::size_t) {
            if (ec) {
                stop("App-server connection closed.");
                return;
            }
            if (!ws.got_text()) {
                stop("Unexpected binary WebSocket message.");
                return;
            }
            const auto length = static_cast<std::uint32_t>(incoming.size());
            outgoing.clear();
            for (int shift = 24; shift >= 0; shift -= 8)
                outgoing.push_back(static_cast<char>(length >> shift));
            outgoing += beast::buffers_to_string(incoming.data());
            incoming.consume(incoming.size());
            asio::async_write(output, asio::buffer(outgoing), [this](auto error, std::size_t) {
                if (error) {
                    stop();
                    return;
                }
                readSocket();
            });
        });
    }

  public:
    ~Bridge() {
        if (child > 0) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
        }
    }
    int run(const std::string &path, const std::string &codex, bool host) {
        if (host || path.empty()) {
            int fds[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds))
                throw std::runtime_error("socketpair failed");
            std::vector<std::string> args;
            if (host)
                args = {"/usr/bin/flatpak-spawn", "--watch-bus", "--host", codex};
            else
                args = {codex};
            args.insert(args.end(), {"app-server", "proxy"});
            if (!path.empty())
                args.insert(args.end(), {"--sock", path});
            std::vector<char *> argv;
            for (auto &a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDIN_FILENO);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, fds[0]);
            posix_spawn_file_actions_addclose(&actions, fds[1]);
            const int rc = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            ::close(fds[1]);
            if (rc) {
                ::close(fds[0]);
                child = -1;
                throw std::runtime_error("Cannot start Codex socket proxy.");
            }
            ws.next_layer().assign(asio::local::stream_protocol(), fds[0]);
        } else
            ws.next_layer().connect(asio::local::stream_protocol::endpoint(path));
        ws.read_message_max(Limit);
        ws.text(true);
        // Only a finite connect deadline; no steady-state heartbeat or polling.
        deadline.expires_after(std::chrono::seconds(15));
        deadline.async_wait([this](auto ec) {
            if (!ec)
                stop("WebSocket handshake timed out.");
        });
        signals.async_wait([this](auto, int) { stop(); });
        ws.async_handshake("localhost", "/", [this](auto ec) {
            deadline.cancel();
            if (ec) {
                stop("App-server WebSocket handshake failed.");
                return;
            }
            readInput();
            readSocket();
        });
        io.run();
        return result;
    }
};
int main(int argc, char **argv) {
    ::signal(SIGPIPE, SIG_IGN);
    std::string path, codex = "codex";
    bool host = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host-proxy")
            host = true;
        else if (arg == "--socket" && i + 1 < argc)
            path = argv[++i];
        else if (arg == "--codex" && i + 1 < argc)
            codex = argv[++i];
        else {
            std::cerr << "Invalid bridge arguments.\n";
            return 2;
        }
    }
    try {
        Bridge b;
        return b.run(path, codex, host);
    } catch (const std::exception &) {
        std::cerr << "Cannot connect to the configured app-server socket.\n";
        return 1;
    }
}
