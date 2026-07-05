// ch05 §5.7: "the concrete way a safe function produces a pointer value
// for an extern "C" out-parameter" -- the chapter's own motivating
// example (getsockopt's SO_TYPE out-parameters), adapted here for a UDP
// (SOCK_DGRAM=2) socket instead of the TCP example in ch05/tests. Real
// Linux constants: AF_INET=2, SOCK_DGRAM=2, SOL_SOCKET=1, SO_TYPE=3.
extern "C" {
    int socket(int domain, int type, int protocol);
    int getsockopt(int fd, int level, int optname, void* optval, int* optlen);
    int close(int fd);
}

safe int query_socket_type(int fd) {
    int value = 0;
    int len = 4;
    unsafe {
        getsockopt(fd, 1, 3, &value, &len);
    }
    return value;
}

int main() {
    int fd = socket(2, 2, 0);
    int socket_type = query_socket_type(fd);
    close(fd);
    return socket_type;
}
