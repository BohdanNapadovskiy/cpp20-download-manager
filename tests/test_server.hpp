#pragma once

#include <httplib.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace dm::test {
    // Plain HTTP server on a free port of 127.0.0.1, running in a background thread for the
    // lifetime of the object. Register handlers through server() BEFORE calling start().
    class TestServer {
    public:
        TestServer() = default;
        TestServer(const TestServer&) = delete;
        TestServer& operator=(const TestServer&) = delete;

        ~TestServer() { stop(); }

        httplib::Server& server() { return server_; }

        void start() {
            port_ = server_.bind_to_any_port("127.0.0.1");
            if (port_ <= 0) {
                throw std::runtime_error("TestServer: cannot bind to a free port");
            }
            thread_ = std::thread([this] { server_.listen_after_bind(); });
            server_.wait_until_ready();
        }

        void stop() {
            if (thread_.joinable()) {
                server_.stop();
                thread_.join();
            }
        }

        // Full URL for a path on this server, e.g. url("/file") -> "http://127.0.0.1:54321/file".
        std::string url(const std::string& path) const {
            return "http://127.0.0.1:" + std::to_string(port_) + path;
        }

    private:
        httplib::Server server_;
        std::thread thread_;
        int port_ = 0;
    };

    // Deterministic pseudo-random binary content: same seed -> same bytes on every run.
    // Binary (not text) so that any newline conversion or truncation at '\0' shows up.
    inline std::string make_content(std::size_t size, std::uint32_t seed = 42) {
        std::mt19937 engine(seed);
        std::string content(size, '\0');
        for (auto& byte : content) {
            byte = static_cast<char>(engine() & 0xFFu);
        }
        return content;
    }
}
