export module httpserver:platform;

import std;

extern "C" {
    union [[scpp::packed]] epoll_data_t {
        void* ptr;
        int fd;
        uint32_t u32;
        uint64_t u64;
    };

    struct [[scpp::packed]] epoll_event {
        uint32_t events;
        epoll_data_t data;
    };

    struct in_addr {
        uint32_t s_addr;
    };

    struct sockaddr {
        uint16_t sa_family;
        char sa_data[14];
    };

    struct sockaddr_in {
        uint16_t sin_family;
        uint16_t sin_port;
        in_addr sin_addr;
        char sin_zero[8];
    };

    int socket(int domain, int type, int protocol);
    int setsockopt(int socket_fd, int level, int option_name, const void* option_value, unsigned int option_len);
    uint32_t htonl(uint32_t hostlong);
    uint16_t htons(uint16_t hostshort);
    int bind(int socket_fd, sockaddr* address, unsigned int address_len);
    int listen(int socket_fd, int backlog);
    int accept4(int socket_fd, void* address, unsigned int* address_len, int flags);
    int close(int fd);

    int epoll_create1(int flags);
    int epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event);
    int epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms);

    long read(int fd, char* buffer, unsigned long count);
    long send(int fd, const char* buffer, unsigned long count, int flags);

    int open(const char* path, int flags);
    int openat(int dir_fd, const char* path, int flags);

    long sendfile(int out_fd, int in_fd, long* offset, unsigned long count);
    long pread(int fd, char* buffer, unsigned long count, long offset);
    long lseek(int fd, long offset, int whence);

    int* __errno_location();
}

namespace httpserver {

export struct PlatformEvent {
    int token;
    int readable;
    int writable;
    int error;
    int hangup;
};

int negative_errno() {
    int* err_ptr{};
    [[scpp::unsafe]] { err_ptr = __errno_location(); }
    int err_value{};
    [[scpp::unsafe]] { err_value = *err_ptr; }
    return 0 - err_value;
}

int af_inet() { return 2; }
int sock_stream() { return 1; }
int sock_nonblock() { return 2048; }
int sock_cloexec() { return 524288; }
int sol_socket() { return 1; }
int so_reuseaddr() { return 2; }
int so_reuseport() { return 15; }
int msg_nosignal() { return 16384; }
int epoll_cloexec() { return 524288; }
int epoll_ctl_add() { return 1; }
int epoll_ctl_del() { return 2; }
int epoll_ctl_mod() { return 3; }
int epollin_flag() { return 1; }
int epollout_flag() { return 4; }
int epollerr_flag() { return 8; }
int epollhup_flag() { return 16; }
int epollrdhup_flag() { return 8192; }
int o_rdonly() { return 0; }
int o_directory() { return 65536; }
int o_nofollow() { return 131072; }
int o_cloexec() { return 524288; }
int seek_set() { return 0; }
int seek_end() { return 2; }
int has_flag(int bits, int flag) {
    int quotient = bits / flag;
    if ((quotient - (quotient / 2) * 2) != 0) return 1;
    return 0;
}

export int errno_eagain() { return 11; }
export int errno_eintr() { return 4; }
export int errno_enoent() { return 2; }
export int errno_eacces() { return 13; }
export int errno_eperm() { return 1; }
export int errno_einval() { return 22; }
export int errno_enotdir() { return 20; }
export int errno_eisdir() { return 21; }
export int errno_eloop() { return 40; }
export int errno_econnreset() { return 104; }
export int errno_epipe() { return 32; }
export int errno_eoverflow() { return 75; }

int scpp_httpserver_close_fd(int fd) {
    if (fd >= 0) [[scpp::unsafe]] { close(fd); }
    return 0;
}

export int close_fd(int fd) { return scpp_httpserver_close_fd(fd); }

int scpp_httpserver_listen_any(int port, int backlog) {
    int fd{};
    [[scpp::unsafe]] { fd = socket(af_inet(), sock_stream() + sock_nonblock() + sock_cloexec(), 0); }
    if (fd < 0) return negative_errno();
    int one = 1;
    [[scpp::unsafe]] {
        setsockopt(fd, sol_socket(), so_reuseaddr(), &one, 4);
        setsockopt(fd, sol_socket(), so_reuseport(), &one, 4);
    }
    sockaddr_in addr{};
    addr.sin_family = static_cast<uint16_t>(af_inet());
    [[scpp::unsafe]] {
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(0));
    }
    int i = 0;
    while (i < 8) {
        addr.sin_zero[i] = '\0';
        i = i + 1;
    }
    int bind_result{};
    [[scpp::unsafe]] {
        bind_result = bind(fd, static_cast<sockaddr*>(static_cast<void*>(&addr)),
                           static_cast<unsigned int>(16));
    }
    if (bind_result != 0) {
        int err = negative_errno();
        scpp_httpserver_close_fd(fd);
        return err;
    }
    int listen_result{};
    [[scpp::unsafe]] { listen_result = listen(fd, backlog); }
    if (listen_result != 0) {
        int err = negative_errno();
        scpp_httpserver_close_fd(fd);
        return err;
    }
    return fd;
}

int scpp_httpserver_accept_nonblocking(int listener_fd) {
    sockaddr dummy_addr{};
    unsigned int dummy_len = 16;
    int fd{};
    [[scpp::unsafe]] { fd = accept4(listener_fd, &dummy_addr, &dummy_len, sock_nonblock() + sock_cloexec()); }
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_epoll_create() {
    int fd{};
    [[scpp::unsafe]] { fd = epoll_create1(epoll_cloexec()); }
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_epoll_add(int epoll_fd, int fd, int token, int readable, int writable) {
    epoll_event ev{};
    int flags = 0;
    if (readable != 0) flags = flags + epollin_flag();
    if (writable != 0) flags = flags + epollout_flag();
    flags = flags + epollerr_flag() + epollhup_flag() + epollrdhup_flag();
    ev.events = static_cast<uint32_t>(flags);
    [[scpp::unsafe]] { ev.data.u32 = static_cast<uint32_t>(token); }
    int result{};
    [[scpp::unsafe]] { result = epoll_ctl(epoll_fd, epoll_ctl_add(), fd, &ev); }
    if (result != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_mod(int epoll_fd, int fd, int token, int readable, int writable) {
    epoll_event ev{};
    int flags = 0;
    if (readable != 0) flags = flags + epollin_flag();
    if (writable != 0) flags = flags + epollout_flag();
    flags = flags + epollerr_flag() + epollhup_flag() + epollrdhup_flag();
    ev.events = static_cast<uint32_t>(flags);
    [[scpp::unsafe]] { ev.data.u32 = static_cast<uint32_t>(token); }
    int result{};
    [[scpp::unsafe]] { result = epoll_ctl(epoll_fd, epoll_ctl_mod(), fd, &ev); }
    if (result != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_delete(int epoll_fd, int fd) {
    epoll_event ev{};
    ev.events = static_cast<uint32_t>(0);
    [[scpp::unsafe]] { ev.data.u32 = static_cast<uint32_t>(0); }
    int result{};
    [[scpp::unsafe]] { result = epoll_ctl(epoll_fd, epoll_ctl_del(), fd, &ev); }
    if (result != 0) return negative_errno();
    return 0;
}

int scpp_httpserver_epoll_wait(int epoll_fd, PlatformEvent* events, int max_events, int timeout_ms) {
    epoll_event native_events[64];
    if (max_events > 64) max_events = 64;
    int ready{};
    [[scpp::unsafe]] { ready = epoll_wait(epoll_fd, &native_events[0], max_events, timeout_ms); }
    if (ready < 0) return negative_errno();
    int i = 0;
    // spec §5.1(5.1): `events` is a raw `PlatformEvent*`, so indexing it
    // is gated exactly as the `native_events` reads below already were.
    [[scpp::unsafe]] {
    while (i < ready) {
        int native_flags{};
        int token{};
        [[scpp::unsafe]] {
            native_flags = static_cast<int>(native_events[i].events);
            token = static_cast<int>(native_events[i].data.u32);
        }
        events[i].token = token;
        events[i].readable = has_flag(native_flags, epollin_flag());
        events[i].writable = has_flag(native_flags, epollout_flag());
        events[i].error = has_flag(native_flags, epollerr_flag());
        events[i].hangup = 0;
        if (has_flag(native_flags, epollhup_flag()) != 0 || has_flag(native_flags, epollrdhup_flag()) != 0) {
            events[i].hangup = 1;
        }
        i = i + 1;
    }
    }
    return ready;
}

int scpp_httpserver_socket_read(int fd, char* buffer, int count) {
    long n{};
    [[scpp::unsafe]] { n = read(fd, buffer, static_cast<unsigned long>(count)); }
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

int scpp_httpserver_socket_write(int fd, const char* buffer, int count) {
    long n{};
    [[scpp::unsafe]] { n = send(fd, buffer, static_cast<unsigned long>(count), msg_nosignal()); }
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

int scpp_httpserver_socket_write(int fd, std::string_view data) {
    long n{};
    [[scpp::unsafe]] { n = send(fd, data.data(), static_cast<unsigned long>(data.size()), msg_nosignal()); }
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

int scpp_httpserver_open_root_directory(const std::string& path) {
    int fd{};
    [[scpp::unsafe]] { fd = open(path.c_str(), o_rdonly() + o_directory() + o_cloexec()); }
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_open_subdirectory(int dir_fd, const std::string& path) {
    int fd{};
    [[scpp::unsafe]] { fd = openat(dir_fd, path.c_str(), o_rdonly() + o_directory() + o_nofollow() + o_cloexec()); }
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_open_file_readonly(int dir_fd, const std::string& path) {
    int fd{};
    [[scpp::unsafe]] { fd = openat(dir_fd, path.c_str(), o_rdonly() + o_nofollow() + o_cloexec()); }
    if (fd < 0) return negative_errno();
    return fd;
}

int scpp_httpserver_file_size(int fd) {
    long size{};
    [[scpp::unsafe]] { size = lseek(fd, static_cast<long>(0), seek_end()); }
    if (size < 0) return negative_errno();
    [[scpp::unsafe]] { lseek(fd, static_cast<long>(0), seek_set()); }
    if (size > static_cast<long>(2147483647)) return 0 - errno_eoverflow();
    return static_cast<int>(size);
}

int scpp_httpserver_sendfile(int socket_fd, int file_fd, int* offset, int count) {
    int offset_value{};
    [[scpp::unsafe]] { offset_value = *offset; }
    long native_offset = static_cast<long>(offset_value);
    long n{};
    [[scpp::unsafe]] {
        n = sendfile(socket_fd, file_fd, &native_offset, static_cast<unsigned long>(count));
    }
    if (n < 0) return negative_errno();
    [[scpp::unsafe]] { *offset = static_cast<int>(native_offset); }
    return static_cast<int>(n);
}

int scpp_httpserver_pread(int file_fd, char* buffer, int count, int offset) {
    long n{};
    [[scpp::unsafe]] {
        n = pread(file_fd, buffer, static_cast<unsigned long>(count), static_cast<long>(offset));
    }
    if (n < 0) return negative_errno();
    return static_cast<int>(n);
}

export bool ends_with(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) return false;
    return text.substr(text.size() - suffix.size()) == suffix;
}

export bool is_dot_segment(std::string_view text) {
    return text == "." || text == "..";
}

export bool contains_forbidden_segment_char(std::string_view text) {
    return text.find('/') != std::string_view::npos ||
           text.find('\\') != std::string_view::npos ||
           text.find('\0') != std::string_view::npos;
}

export int hex_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c) - static_cast<int>('0');
    if (c >= 'a' && c <= 'f') return 10 + static_cast<int>(c) - static_cast<int>('a');
    if (c >= 'A' && c <= 'F') return 10 + static_cast<int>(c) - static_cast<int>('A');
    return -1;
}

export bool decode_request_path(std::string_view raw_target, std::string& out, size_t max_len) {
    out.clear();
    if (raw_target.empty() || raw_target.at(0) != '/') return false;
    std::string decoded_path{};
    size_t i = 1;
    while (i < raw_target.size()) {
        char c = raw_target.at(i);
        if (c == '?') break;
        if (c == '\0' || c == '\\') return false;
        if (c == '%') {
            if (i + 2 >= raw_target.size()) return false;
            int hi = hex_value(raw_target.at(i + 1));
            int lo = hex_value(raw_target.at(i + 2));
            if (hi < 0 || lo < 0) return false;
            char decoded = static_cast<char>(hi * 16 + lo);
            if (decoded == '\0' || decoded == '/' || decoded == '\\') return false;
            if (decoded_path.size() >= max_len) return false;
            decoded_path.push_back(decoded);
            i = i + 3;
            continue;
        }
        if (decoded_path.size() >= max_len) return false;
        decoded_path.push_back(c);
        i = i + 1;
    }
    size_t trimmed_len = decoded_path.size();
    while (trimmed_len > 0 && decoded_path.at(trimmed_len - 1) == '/') {
        trimmed_len = trimmed_len - 1;
    }
    if (trimmed_len == decoded_path.size()) {
        out = std::move(decoded_path);
    } else {
        out = decoded_path.substr(0, trimmed_len);
    }
    return true;
}

export std::string_view content_type_for(std::string_view relative_path) {
    if (relative_path.empty() || ends_with(relative_path, ".html")) return "text/html";
    if (ends_with(relative_path, ".txt")) return "text/plain";
    if (ends_with(relative_path, ".css")) return "text/css";
    if (ends_with(relative_path, ".js")) return "application/javascript";
    if (ends_with(relative_path, ".json")) return "application/json";
    if (ends_with(relative_path, ".xml")) return "application/xml";
    if (ends_with(relative_path, ".png")) return "image/png";
    if (ends_with(relative_path, ".jpg") || ends_with(relative_path, ".jpeg")) return "image/jpeg";
    if (ends_with(relative_path, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

export class Connection {
    public:
        virtual ~Connection() { return; }
public:
    int fd_{};
    std::string request_{};
    size_t response_sent_{};
    std::string response_{};
    int file_fd_{};
    int file_size_{};
    int file_offset_{};
    bool head_only_{};
    bool use_sendfile_{};
    std::string relative_path_{};
    std::string_view content_type_{};
    char file_buffer_[4096];
    int file_buffer_len_{};
    int file_buffer_sent_{};

    void initialize_empty() {
        this->fd_ = -1;
        this->request_.clear();
        this->response_sent_ = 0;
        this->response_.clear();
        this->file_fd_ = -1;
        this->file_size_ = 0;
        this->file_offset_ = 0;
        this->head_only_ = false;
        this->use_sendfile_ = true;
        this->relative_path_.clear();
        this->content_type_ = std::string_view{"application/octet-stream"};
        this->file_buffer_len_ = 0;
        this->file_buffer_sent_ = 0;
        return;
    }

    Connection() {
        this->initialize_empty();
        return;
    }

    void reset() {
        if (this->fd_ >= 0) close_fd(this->fd_);
        if (this->file_fd_ >= 0) close_fd(this->file_fd_);
        this->initialize_empty();
        return;
    }
};

} // namespace httpserver
