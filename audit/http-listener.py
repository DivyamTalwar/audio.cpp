"""Audit controller; only source/test/CMake changes reach the fix branch."""
from pathlib import Path
import sys

SOURCE = Path('app/server/http.cpp')
TEST = Path('tests/unittests/test_http_live_body.cpp')
CMAKE = Path('CMakeLists.txt')

PRESSURE = r'''
#ifndef _WIN32
// Keep every low descriptor occupied before serve_http creates its listener.
// This exercises the public server path, not a copy of its readiness helper.
class DescriptorPressure {
public:
    DescriptorPressure() = default;
    DescriptorPressure(const DescriptorPressure &) = delete;
    DescriptorPressure & operator=(const DescriptorPressure &) = delete;
    ~DescriptorPressure() {
        for (const int fd : descriptors_) close(fd);
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
                std::cout << "HTTP listener descriptor will exceed " << fd << std::endl;
                return true;
            }
        }
    }

private:
    std::vector<int> descriptors_;
};
#endif
'''
SETUP = r'''
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--high-fd")) {
        std::cerr << "usage: http_live_body_test [--high-fd]\n";
        return 2;
    }
#ifndef _WIN32
    DescriptorPressure pressure;
    if (argc == 2 && !pressure.reserve()) {
        std::cout << "SKIP: insufficient descriptors for the high-fd listener regression\n";
        return 77;
    }
#else
    if (argc == 2) {
        std::cout << "SKIP: numeric FD_SETSIZE bounds are POSIX-specific\n";
        return 77;
    }
#endif
'''
FIX = r'''bool wait_for_client(SocketHandle socket, int timeout_ms) {
#ifdef _WIN32
    // Windows fd_set stores socket handles rather than indexing by their value.
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
    // FD_SETSIZE would write past that bitmap; poll has no such numeric limit.
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
    text = TEST.read_text()
    assert 'DescriptorPressure' not in text
    text = text.replace('#include <sys/socket.h>', '#include <sys/socket.h>\n#include <sys/select.h>\n#include <sys/resource.h>\n#include <fcntl.h>')
    marker = 'using engine::test::require_eq;'
    assert text.count(marker) == 1
    text = text.replace(marker, marker + '\n' + PRESSURE)
    marker = 'int main() {'
    assert text.count(marker) == 1
    text = text.replace(marker, 'int main(int argc, char ** argv) {' + '\n' + SETUP)
    TEST.write_text(text)
    text = CMAKE.read_text()
    marker = '        set_tests_properties(http_live_body_test PROPERTIES TIMEOUT 120)'
    assert text.count(marker) == 1
    text = text.replace(marker, marker + '''
        if(NOT WIN32)
            add_test(NAME http_listener_high_fd_test COMMAND http_live_body_test --high-fd)
            set_tests_properties(http_listener_high_fd_test PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
            # Both invocations use the same loopback port; never run them together.
            set_tests_properties(http_live_body_test http_listener_high_fd_test
                PROPERTIES RESOURCE_LOCK http_live_body_port)
        endif()''')
    CMAKE.write_text(text)
elif sys.argv[1] == 'fix':
    text = SOURCE.read_text()
    start = text.index('bool wait_for_client(SocketHandle socket, int timeout_ms) {')
    end = text.index('\n}', start) + 2
    old = text[start:end]
    assert 'select(socket + 1' in old
    text = text[:start] + FIX + text[end:]
    text = text.replace('#include <sys/select.h>\n', '')
    SOURCE.write_text(text)
else:
    raise SystemExit('expected test-only or fix')
