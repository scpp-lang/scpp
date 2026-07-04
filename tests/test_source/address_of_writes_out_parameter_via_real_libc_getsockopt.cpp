extern "C" {
    int socket(int domain, int type, int protocol);
    int getsockopt(int fd, int level, int optname, void* optval, int* optlen);
    int close(int fd);
}

// SOCK_STREAM=1, AF_INET=2, SOL_SOCKET=1, SO_TYPE=3 (fixed Linux constants).
// This is the concrete motivating case for &expr (ch05 §5.7): getsockopt's
// "out" parameters need a pointer to the *caller's own* storage, which
// &value/&len produce -- there is no other way to obtain one in this
// version.
safe int query_socket_type(int fd) {
    int value = 0;
    int len = 4;
    unsafe {
        getsockopt(fd, 1, 3, &value, &len);
    }
    return value;
}

int main() {
    int fd = 0;
    unsafe {
        fd = socket(2, 1, 0);
    }
    print_int(query_socket_type(fd));
    unsafe {
        close(fd);
    }
    return 0;
}
