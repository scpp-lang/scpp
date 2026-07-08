#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {
struct ScppHttpserverEvent {
    int token;
    int readable;
    int writable;
    int error;
    int hangup;
};

int negative_errno() { return errno == 0 ? -1 : -errno; }

bool is_dot_segment(const char* segment, int len) {
    return (len == 1 && segment[0] == '.') || (len == 2 && segment[0] == '.' && segment[1] == '.');
}

bool contains_forbidden_segment_char(const char* segment, int len) {
    for (int i = 0; i < len; ++i) {
        if (segment[i] == '\0' || segment[i] == '/' || segment[i] == '\\') return true;
    }
    return false;
}

int open_index_file(int dir_fd, int* out_size) {
    int file_fd = openat(dir_fd, "index.html", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file_fd < 0) return negative_errno();
    struct stat st {};
    if (fstat(file_fd, &st) != 0) {
        int err = negative_errno();
        close(file_fd);
        return err;
    }
    if (!S_ISREG(st.st_mode) || st.st_size > INT_MAX) {
        close(file_fd);
        errno = EINVAL;
        return negative_errno();
    }
    *out_size = static_cast<int>(st.st_size);
    return file_fd;
}

int open_static_file_impl(int root_fd, const char* path, int allow_index_html, int deny_hidden_files, int* out_size) {
    int current_fd = dup(root_fd);
    if (current_fd < 0) return negative_errno();

    const char* cursor = path;
    bool consumed_any = false;
    while (cursor[0] != '\0') {
        const char* slash = cursor;
        while (*slash != '\0' && *slash != '/') ++slash;
        int len = static_cast<int>(slash - cursor);
        if (len == 0 || is_dot_segment(cursor, len) || contains_forbidden_segment_char(cursor, len)) {
            close(current_fd);
            errno = EINVAL;
            return negative_errno();
        }
        if (deny_hidden_files && cursor[0] == '.') {
            close(current_fd);
            errno = EACCES;
            return negative_errno();
        }
        consumed_any = true;
        bool last = *slash == '\0';
        std::string segment(cursor, cursor + len);
        if (!last) {
            int next_fd = openat(current_fd, segment.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next_fd < 0) {
                int err = negative_errno();
                close(current_fd);
                return err;
            }
            close(current_fd);
            current_fd = next_fd;
            cursor = slash + 1;
            if (*cursor == '\0') {
                if (!allow_index_html) {
                    close(current_fd);
                    errno = EISDIR;
                    return negative_errno();
                }
                int index_fd = open_index_file(current_fd, out_size);
                close(current_fd);
                return index_fd;
            }
            continue;
        }

        int file_fd = openat(current_fd, segment.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (file_fd < 0) {
            int err = negative_errno();
            close(current_fd);
            return err;
        }
        struct stat st {};
        if (fstat(file_fd, &st) != 0) {
            int err = negative_errno();
            close(file_fd);
            close(current_fd);
            return err;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!allow_index_html) {
                close(file_fd);
                close(current_fd);
                errno = EISDIR;
                return negative_errno();
            }
            int index_fd = open_index_file(file_fd, out_size);
            close(file_fd);
            close(current_fd);
            return index_fd;
        }
        if (!S_ISREG(st.st_mode) || st.st_size > INT_MAX) {
            close(file_fd);
            close(current_fd);
            errno = EINVAL;
            return negative_errno();
        }
        *out_size = static_cast<int>(st.st_size);
        close(current_fd);
        return file_fd;
    }

    if (!consumed_any) {
        if (!allow_index_html) {
            close(current_fd);
            errno = EISDIR;
            return negative_errno();
        }
        int index_fd = open_index_file(current_fd, out_size);
        close(current_fd);
        return index_fd;
    }

    close(current_fd);
    errno = EINVAL;
    return negative_errno();
}

bool decode_request_path(std::string_view source, std::string& out) {
    out.clear();
    if (source.empty() || source.front() != '/') return false;
    for (size_t i = 0; i < source.size(); ++i) {
        char c = source[i];
        if (c == '?') break;
        if (c == '%') {
            if (i + 2 >= source.size()) return false;
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
                if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
                return -1;
            };
            int hi = hex(source[i + 1]);
            int lo = hex(source[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char decoded = static_cast<char>((hi << 4) | lo);
            if (decoded == 0 || decoded == '/' || decoded == '\\') return false;
            out.push_back(decoded);
            i += 2;
            continue;
        }
        if (c == '\\') return false;
        out.push_back(c);
    }
    return !out.empty() && out.front() == '/';
}

const char* content_type_for(std::string_view path) {
    auto ends_with = [&](std::string_view suffix) {
        return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
    };
    if (ends_with(".html")) return "text/html";
    if (ends_with(".txt")) return "text/plain";
    if (ends_with(".css")) return "text/css";
    if (ends_with(".js")) return "application/javascript";
    if (ends_with(".json")) return "application/json";
    if (ends_with(".png")) return "image/png";
    if (ends_with(".jpg") || ends_with(".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

struct Connection {
    int fd = -1;
    std::string request;
    std::string response_headers;
    int file_fd = -1;
    int file_size = 0;
    int file_offset = 0;
    bool head_only = false;
    bool use_sendfile = true;
    std::string decoded_path;
    std::string relative_path;
    std::string content_type = "application/octet-stream";
    char file_buffer[4096]{};
    int file_buffer_len = 0;
    int file_buffer_sent = 0;

    void reset() {
        if (fd >= 0) close(fd);
        if (file_fd >= 0) close(file_fd);
        fd = -1;
        file_fd = -1;
        file_size = 0;
        file_offset = 0;
        head_only = false;
        use_sendfile = true;
        request.clear();
        response_headers.clear();
        decoded_path.clear();
        relative_path.clear();
        content_type = "application/octet-stream";
        file_buffer_len = 0;
        file_buffer_sent = 0;
    }
};

void close_connection(int epoll_fd, Connection& conn) {
    if (conn.fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
    }
    conn.reset();
}

bool parse_request(Connection& conn, int root_fd, int allow_index_html, int deny_hidden_files) {
    auto header_end = conn.request.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    auto line_end = conn.request.find("\r\n");
    if (line_end == std::string::npos) {
        conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }
    std::string_view request_line(conn.request.data(), line_end);
    auto first_space = request_line.find(' ');
    auto second_space = request_line.rfind(' ');
    if (first_space == std::string_view::npos || second_space == std::string_view::npos || first_space == second_space) {
        conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }
    std::string_view method = request_line.substr(0, first_space);
    std::string_view target = request_line.substr(first_space + 1, second_space - first_space - 1);
    std::string_view version = request_line.substr(second_space + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }
    if (method == "GET") conn.head_only = false;
    else if (method == "HEAD") conn.head_only = true;
    else {
        conn.response_headers = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }
    if (!decode_request_path(target, conn.decoded_path)) {
        conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }
    conn.relative_path = conn.decoded_path == "/" ? "" : conn.decoded_path.substr(1);
    while (!conn.relative_path.empty() && conn.relative_path.back() == '/') conn.relative_path.pop_back();

    int file_size = 0;
    int file_fd = open_static_file_impl(root_fd, conn.relative_path.c_str(), allow_index_html, deny_hidden_files, &file_size);
    if (file_fd < 0) {
        int err = -file_fd;
        if (err == EINVAL) conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        else if (err == ENOENT || err == ENOTDIR) conn.response_headers = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        else conn.response_headers = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        return true;
    }

    conn.file_fd = file_fd;
    conn.file_size = file_size;
    conn.file_offset = 0;
    conn.content_type = content_type_for(conn.relative_path);
    conn.response_headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(conn.file_size) +
                            "\r\nContent-Type: " + conn.content_type + "\r\nConnection: close\r\n\r\n";
    return true;
}
}

extern "C" {
const char* scpp_httpserver_getenv_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    return value;
}

int scpp_httpserver_getenv_int_or(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > INT_MAX) return fallback;
    return static_cast<int>(parsed);
}

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
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return negative_errno();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr {};
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
    epoll_event ev {};
    ev.data.u32 = static_cast<uint32_t>(token);
    if (readable) ev.events |= EPOLLIN;
    if (writable) ev.events |= EPOLLOUT;
    ev.events |= EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_mod(int epoll_fd, int fd, int token, int readable, int writable) {
    epoll_event ev {};
    ev.data.u32 = static_cast<uint32_t>(token);
    if (readable) ev.events |= EPOLLIN;
    if (writable) ev.events |= EPOLLOUT;
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
    epoll_event native_events[64] {};
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

int scpp_httpserver_open_static_file(int root_fd, const char* path, int allow_index_html,
                                     int deny_hidden_files, int* out_size) {
    return open_static_file_impl(root_fd, path, allow_index_html, deny_hidden_files, out_size);
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

int scpp_httpserver_run(const char* root, int port, int max_connections, int allow_index_html, int deny_hidden_files) {
    int root_fd = scpp_httpserver_open_root_directory(root);
    if (root_fd < 0) return 1;
    int listener_fd = scpp_httpserver_listen_any(port, 128);
    if (listener_fd < 0) {
        close(root_fd);
        return 1;
    }
    int epoll_fd = scpp_httpserver_epoll_create();
    if (epoll_fd < 0) {
        close(listener_fd);
        close(root_fd);
        return 1;
    }
    if (scpp_httpserver_epoll_add(epoll_fd, listener_fd, 0, 1, 0) < 0) {
        close(epoll_fd);
        close(listener_fd);
        close(root_fd);
        return 1;
    }

    if (max_connections <= 0) max_connections = 32;
    std::vector<Connection> connections(static_cast<size_t>(max_connections));
    ScppHttpserverEvent events[64]{};

    for (;;) {
        int ready = scpp_httpserver_epoll_wait(epoll_fd, events, 64, -1);
        if (ready < 0) continue;
        for (int i = 0; i < ready; ++i) {
            if (events[i].token == 0) {
                for (;;) {
                    int fd = scpp_httpserver_accept_nonblocking(listener_fd);
                    if (fd < 0) {
                        if (-fd == EAGAIN || -fd == EINTR) break;
                        break;
                    }
                    int slot = -1;
                    for (int j = 0; j < max_connections; ++j) {
                        if (connections[static_cast<size_t>(j)].fd < 0) {
                            slot = j;
                            break;
                        }
                    }
                    if (slot < 0) {
                        close(fd);
                        continue;
                    }
                    auto& conn = connections[static_cast<size_t>(slot)];
                    conn.reset();
                    conn.fd = fd;
                    scpp_httpserver_epoll_add(epoll_fd, fd, slot + 1, 1, 0);
                }
                continue;
            }

            int slot = events[i].token - 1;
            if (slot < 0 || slot >= max_connections) continue;
            auto& conn = connections[static_cast<size_t>(slot)];
            if (conn.fd < 0) continue;

            if (events[i].error || events[i].hangup) {
                close_connection(epoll_fd, conn);
                continue;
            }

            if (events[i].readable) {
                char buffer[2048];
                for (;;) {
                    int n = scpp_httpserver_socket_read(conn.fd, buffer, sizeof(buffer));
                    if (n == 0) {
                        close_connection(epoll_fd, conn);
                        break;
                    }
                    if (n < 0) {
                        if (-n == EAGAIN || -n == EINTR) break;
                        close_connection(epoll_fd, conn);
                        break;
                    }
                    conn.request.append(buffer, static_cast<size_t>(n));
                    if (conn.request.size() > 8192) {
                        conn.response_headers = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
                        scpp_httpserver_epoll_mod(epoll_fd, conn.fd, slot + 1, 0, 1);
                        break;
                    }
                    if (conn.request.find("\r\n\r\n") != std::string::npos) {
                        parse_request(conn, root_fd, allow_index_html, deny_hidden_files);
                        scpp_httpserver_epoll_mod(epoll_fd, conn.fd, slot + 1, 0, 1);
                        break;
                    }
                }
            }

            if (conn.fd < 0) continue;

            if (events[i].writable) {
                while (!conn.response_headers.empty()) {
                    int n = scpp_httpserver_socket_write(conn.fd, conn.response_headers.data(),
                                                         static_cast<int>(conn.response_headers.size()));
                    if (n < 0) {
                        if (-n == EAGAIN || -n == EINTR) break;
                        close_connection(epoll_fd, conn);
                        break;
                    }
                    conn.response_headers.erase(0, static_cast<size_t>(n));
                }
                if (conn.fd < 0 || !conn.response_headers.empty()) continue;
                if (conn.head_only || conn.file_fd < 0 || conn.file_size == 0) {
                    close_connection(epoll_fd, conn);
                    continue;
                }
                if (conn.use_sendfile) {
                    int remaining = conn.file_size - conn.file_offset;
                    int sent = scpp_httpserver_sendfile(conn.fd, conn.file_fd, &conn.file_offset, remaining);
                    if (sent < 0) {
                        if (-sent == EAGAIN || -sent == EINTR) continue;
                        conn.use_sendfile = false;
                        conn.file_buffer_len = 0;
                        conn.file_buffer_sent = 0;
                    }
                }
                if (!conn.use_sendfile) {
                    if (conn.file_buffer_sent >= conn.file_buffer_len) {
                        int remaining = conn.file_size - conn.file_offset;
                        int count = remaining > static_cast<int>(sizeof(conn.file_buffer))
                                        ? static_cast<int>(sizeof(conn.file_buffer))
                                        : remaining;
                        int read_count = scpp_httpserver_pread(conn.file_fd, conn.file_buffer, count, conn.file_offset);
                        if (read_count <= 0) {
                            close_connection(epoll_fd, conn);
                            continue;
                        }
                        conn.file_buffer_len = read_count;
                        conn.file_buffer_sent = 0;
                    }
                    int wrote = scpp_httpserver_socket_write(conn.fd, conn.file_buffer + conn.file_buffer_sent,
                                                             conn.file_buffer_len - conn.file_buffer_sent);
                    if (wrote < 0) {
                        if (-wrote == EAGAIN || -wrote == EINTR) continue;
                        close_connection(epoll_fd, conn);
                        continue;
                    }
                    conn.file_buffer_sent += wrote;
                    conn.file_offset += wrote;
                }
                if (conn.file_offset >= conn.file_size) {
                    close_connection(epoll_fd, conn);
                }
            }
        }
    }

    close(epoll_fd);
    close(listener_fd);
    close(root_fd);
    return 0;
}
}
