"""Audit-only controller; only source/test/CMake changes reach the fix branch."""
from pathlib import Path
import sys

SOURCE = Path('app/server/http.cpp')
TEST = Path('tests/unittests/test_http_listener.cpp')
CMAKE = Path('CMakeLists.txt')

TEST_CODE = r'''// Exercise the real POSIX HTTP listener with ordinary and high descriptors.
// No model weights or inference backend are involved in this transport test.
#include "../../app/server/http.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kPort = 18095;
std::atomic<bool> stop{false};
bool stop_requested() { return stop.load(); }

void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}

class DescriptorPressure {
public:
    ~DescriptorPressure() {
        for (int fd : descriptors_) close(fd);
    }
    bool reserve() {
        constexpr int target = FD_SETSIZE + 16;
        rlimit limits{};
        if (getrlimit(RLIMIT_NOFILE, &limits) != 0) return false;
        if (limits.rlim_cur < static_cast<rlim_t>(target + 32)) {
            if (limits.rlim_max < static_cast<rlim_t>(target + 32)) return false;
            limits.rlim_cur = target + 32;
            if (setrlimit(RLIMIT_NOFILE, &limits) != 0) return false;
        }
        for (;;) {
            const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0) return false;
            descriptors_.push_back(fd);
            if (fd >= target) {
                std::cout << "Listener will use a descriptor above " << fd << std::endl;
                return true;
            }
        }
    }
private:
    std::vector<int> descriptors_;
};

class Handler final : public minitts::server::IHttpHandler {
public:
    minitts::server::HttpResponse handle(const minitts::server::HttpRequest &) override {
        return minitts::server::json_response("{\"ok\":true}");
    }
};

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    Socket(const Socket &) = delete;
    Socket & operator=(const Socket &) = delete;
    ~Socket() { if (fd_ >= 0) close(fd_); }
    int get() const { return fd_; }
private:
    int fd_;
};

int connect_to_server() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "could not create test socket");
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) return fd;
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("listener did not become available");
}

void exchange() {
    Socket client(connect_to_server());
    timeval timeout{3, 0};
    require(setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "could not set receive timeout");
    require(setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
            "could not set send timeout");
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(client.get(), request.data() + sent, request.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "could not send request");
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[1024];
    for (;;) {
        const ssize_t n = recv(client.get(), buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, "response timed out or failed");
        if (n == 0) break;
        response.append(buffer, static_cast<size_t>(n));
        require(response.size() < 8192, "response exceeded test bound");
    }
    require(response.find("HTTP/1.1 200 OK") == 0, "request was not accepted");
    require(response.find("{\"ok\":true}") != std::string::npos, "response body was lost");
}
} // namespace

int main(int argc, char ** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--high-fd")) return 2;
    DescriptorPressure pressure;
    if (argc == 2 && !pressure.reserve()) {
        std::cout << "SKIP: cannot reserve descriptors above FD_SETSIZE\n";
        return 77;
    }
    Handler handler;
    std::exception_ptr server_error;
    std::thread server([&] {
        try {
            minitts::server::serve_http("127.0.0.1", kPort, handler, stop_requested, 4096);
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    std::exception_ptr client_error;
    try {
        for (int i = 0; i < 10; ++i) exchange();
    } catch (...) {
        client_error = std::current_exception();
    }
    const auto shutdown_start = std::chrono::steady_clock::now();
    stop.store(true);
    server.join(); // server_error is read only after the writer is joined.
    try {
        if (server_error) std::rethrow_exception(server_error);
        if (client_error) std::rethrow_exception(client_error);
        require(std::chrono::steady_clock::now() - shutdown_start < std::chrono::seconds(3),
                "idle listener did not stop promptly");
        std::cout << "PASS: ten HTTP exchanges and idle shutdown\n";
        return 0;
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''

FIX = r'''bool wait_for_client(SocketHandle socket, int timeout_ms) {
#ifdef _WIN32
    // Windows fd_set stores handles rather than indexing by their numeric value.
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
        if (WSAGetLastError() == WSAEINTR) return false;
        throw std::runtime_error("server select failed");
    }
    return ready > 0 && FD_ISSET(socket, &read_set);
#else
    // POSIX FD_SET indexes a fixed-size bitmap. A valid listener above
    // FD_SETSIZE would write past it; poll has no such numeric limit.
    pollfd descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLIN;
    const int ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) return false;
        throw std::runtime_error("server poll failed");
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("server listener poll reported a socket error");
    }
    return ready > 0 && (descriptor.revents & POLLIN) != 0;
#endif
}'''

if sys.argv[1] == 'test-only':
    assert not TEST.exists()
    TEST.write_text(TEST_CODE)
    text = CMAKE.read_text()
    marker = '        add_executable(http_live_body_test\n'
    assert text.count(marker) == 1
    addition = '''        if(NOT WIN32)
            # Transport-only regression: no model weights or inference runtime.
            add_executable(http_listener_test
                tests/unittests/test_http_listener.cpp
                app/server/http.cpp
                src/framework/debug/trace.cpp
                src/framework/io/json.cpp
                src/framework/io/filesystem.cpp
            )
            target_include_directories(http_listener_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/external/cJSON)
            target_link_libraries(http_listener_test PRIVATE cjson_vendor Threads::Threads)
            add_test(NAME http_listener_test COMMAND http_listener_test)
            add_test(NAME http_listener_high_fd_test COMMAND http_listener_test --high-fd)
            set_tests_properties(http_listener_test http_listener_high_fd_test PROPERTIES
                TIMEOUT 30 RESOURCE_LOCK http_listener_port)
            set_tests_properties(http_listener_high_fd_test PROPERTIES SKIP_RETURN_CODE 77)
        endif()

'''
    CMAKE.write_text(text.replace(marker, addition + marker))
elif sys.argv[1] == 'fix':
    text = SOURCE.read_text()
    start = text.index('bool wait_for_client(SocketHandle socket, int timeout_ms) {')
    end = text.index('\n}', start) + 2
    assert 'select(socket + 1' in text[start:end]
    text = text[:start] + FIX + text[end:]
    text = text.replace('#include <sys/select.h>\n', '')
    SOURCE.write_text(text)
else:
    raise SystemExit('expected test-only or fix')
