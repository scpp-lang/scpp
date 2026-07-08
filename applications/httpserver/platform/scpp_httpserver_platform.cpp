#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
struct ScppHttpserverEvent {
    int token;
    int readable;
    int writable;
    int error;
    int hangup;
};

int negative_errno() { return errno == 0 ? -1 : -errno; }
}

extern "C" {
int scpp_httpserver_errno_eagain() { return EAGAIN; }
int scpp_httpserver_errno_eintr() { return EINTR; }
int scpp_httpserver_errno_enoent() { return ENOENT; }
int scpp_httpserver_errno_eacces() { return EACCES; }
int scpp_httpserver_errno_eperm() { return EPERM; }
int scpp_httpserver_errno_einval() { return EINVAL; }
int scpp_httpserver_errno_enotdir() { return ENOTDIR; }
int scpp_httpserver_errno_eisdir() { return EISDIR; }
int scpp_httpserver_errno_eloop() { return ELOOP; }
int scpp_httpserver_errno_econnreset() { return ECONNRESET; }
int scpp_httpserver_errno_epipe() { return EPIPE; }

int scpp_httpserver_listen_any(int port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return negative_errno();
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int err = negative_errno();
        close(fd);
        return err;
    }
    if (listen(fd, backlog) != 0) {
        int err = negative_errno();
        close(fd);
        return err;
    }
    return fd;
}

int scpp_httpserver_accept_nonblocking(int listener_fd) {
    int fd = accept4(listener_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_close_fd(int fd) {
    if (fd >= 0) close(fd);
    return 0;
}

int scpp_httpserver_epoll_create() {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_epoll_add(int epoll_fd, int fd, int token, int readable, int writable) {
    epoll_event ev{};
    ev.data.u32 = static_cast<uint32_t>(token);
    if (readable != 0) ev.events |= EPOLLIN;
    if (writable != 0) ev.events |= EPOLLOUT;
    ev.events |= EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_mod(int epoll_fd, int fd, int token, int readable, int writable) {
    epoll_event ev{};
    ev.data.u32 = static_cast<uint32_t>(token);
    if (readable != 0) ev.events |= EPOLLIN;
    if (writable != 0) ev.events |= EPOLLOUT;
    ev.events |= EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_delete(int epoll_fd, int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_wait(int epoll_fd, void* events_void, int max_events, int timeout_ms) {
    auto* events = static_cast<ScppHttpserverEvent*>(events_void);
    epoll_event native_events[64]{};
    if (max_events > 64) max_events = 64;
    int ready = epoll_wait(epoll_fd, native_events, max_events, timeout_ms);
    if (ready < 0) return negative_errno();
    for (int i = 0; i < ready; ++i) {
        events[i].token = static_cast<int>(native_events[i].data.u32);
        events[i].readable = (native_events[i].events & EPOLLIN) != 0 ? 1 : 0;
        events[i].writable = (native_events[i].events & EPOLLOUT) != 0 ? 1 : 0;
        events[i].error = (native_events[i].events & EPOLLERR) != 0 ? 1 : 0;
        events[i].hangup = (native_events[i].events & (EPOLLHUP | EPOLLRDHUP)) != 0 ? 1 : 0;
    }
    return ready;
}

int scpp_httpserver_socket_read(int fd, char* buffer, int count) {
    ssize_t n = ::read(fd, buffer, static_cast<size_t>(count));
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

int scpp_httpserver_socket_write(int fd, const char* buffer, int count) {
    ssize_t n = send(fd, buffer, static_cast<size_t>(count), MSG_NOSIGNAL);
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

int scpp_httpserver_open_root_directory(const char* path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_open_subdirectory(int dir_fd, const char* path) {
    int fd = openat(dir_fd, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_open_file_readonly(int dir_fd, const char* path) {
    int fd = openat(dir_fd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_stat_fd(int fd, int* out_is_dir, int* out_is_reg, int* out_size) {
    struct stat st{};
    if (fstat(fd, &st) != 0) return negative_errno();
    *out_is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    *out_is_reg = S_ISREG(st.st_mode) ? 1 : 0;
    if (st.st_size < 0 || st.st_size > INT_MAX) {
        errno = EOVERFLOW;
        return negative_errno();
    }
    *out_size = static_cast<int>(st.st_size);
    return 0;
}

int scpp_httpserver_sendfile(int socket_fd, int file_fd, int* offset, int count) {
    off_t off = static_cast<off_t>(*offset);
    ssize_t n = sendfile(socket_fd, file_fd, &off, static_cast<size_t>(count));
    if (n < 0) return negative_errno();
    *offset = static_cast<int>(off);
    return static_cast<int>(n);
}

int scpp_httpserver_pread(int file_fd, char* buffer, int count, int offset) {
    ssize_t n = pread(file_fd, buffer, static_cast<size_t>(count), static_cast<off_t>(offset));
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}
}
