import std;
import httpserver;

extern "C" {
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
    uint32_t htonl(uint32_t hostlong);
    uint16_t htons(uint16_t hostshort);
    uint16_t ntohs(uint16_t netshort);
    int bind(int socket_fd, sockaddr* address, unsigned int address_len);
    int getsockname(int socket_fd, sockaddr* address, unsigned int* address_len);
    int connect(int socket_fd, sockaddr* address, unsigned int address_len);
    long send(int socket_fd, const char* buffer, unsigned long length, int flags);
    long recv(int socket_fd, char* buffer, unsigned long length, int flags);
    int close(int fd);

    int fork();
    void _exit(int status);
    int kill(int pid, int sig);
    int waitpid(int pid, int* status, int options);
    int usleep(unsigned int usec);
    int getpid();
    int access(const char* pathname, int mode);
    int symlink(const char* target, const char* linkpath);
    int system(const char* command);
}

void expect(bool condition, const char* message, int& failures) {
    if (!condition) {
        std::println("FAILED: {}", message);
        failures = failures + 1;
    }
    return;
}

int reserve_port() {
    int fd = -1;
    [[scpp::unsafe]] { fd = socket(2, 1, 0); }
    if (fd < 0) return 19080;
    sockaddr_in addr{};
    addr.sin_family = static_cast<uint16_t>(2);
    [[scpp::unsafe]] {
        addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(2130706433));
    }
    addr.sin_port = static_cast<uint16_t>(0);
    int i = 0;
    while (i < 8) {
        addr.sin_zero[i] = '\0';
        i = i + 1;
    }
    int bind_rc = -1;
    [[scpp::unsafe]] {
        bind_rc = bind(fd, static_cast<sockaddr*>(static_cast<void*>(&addr)), static_cast<unsigned int>(16));
    }
    if (bind_rc != 0) {
        [[scpp::unsafe]] { close(fd); }
        return 19080;
    }
    unsigned int len = static_cast<unsigned int>(16);
    int name_rc = -1;
    [[scpp::unsafe]] {
        name_rc = getsockname(fd, static_cast<sockaddr*>(static_cast<void*>(&addr)), &len);
    }
    if (name_rc != 0) {
        [[scpp::unsafe]] { close(fd); }
        return 19080;
    }
    uint16_t net_port = addr.sin_port;
    uint16_t host_port = 0;
    [[scpp::unsafe]] {
        host_port = ntohs(net_port);
        close(fd);
    }
    return static_cast<int>(host_port);
}

std::string find_testdata_dir() {
    int ok = -1;
    [[scpp::unsafe]] { ok = access("testdata/index.html", 0); }
    if (ok == 0) return std::string{"testdata"};
    [[scpp::unsafe]] { ok = access("../testdata/index.html", 0); }
    if (ok == 0) return std::string{"../testdata"};
    [[scpp::unsafe]] { ok = access("applications/httpserver/testdata/index.html", 0); }
    if (ok == 0) return std::string{"applications/httpserver/testdata"};
    return std::string{"../../applications/httpserver/testdata"};
}

std::string prepare_docroot(const std::string& work_dir) {
    std::string testdata_dir = find_testdata_dir();
    std::string docroot = work_dir + "/docroot";
    std::string cmd = "rm -rf \"" + work_dir + "\" && mkdir -p \"" + work_dir + "\" && cp -r \"" +
                      testdata_dir + "\" \"" + docroot + "\" && printf 'secret\\n' > \"" + work_dir +
                      "/secret.txt\"";
    std::string link_path = docroot + "/link.txt";
    [[scpp::unsafe]] {
        system(cmd.c_str());
        symlink("hello.txt", link_path.c_str());
    }
    return docroot;
}

int start_server(const std::string& root, int port) {
    int pid = -1;
    [[scpp::unsafe]] { pid = fork(); }
    if (pid != 0) return pid;
    httpserver::ServerBuilder builder{};
    builder.mount("/", root.c_str());
    builder.set_port(port);
    builder.set_max_connections(32);
    builder.set_worker_count(2);
    builder.allow_index_html(true);
    builder.deny_hidden_files(true);
    int rc = builder.serve();
    [[scpp::unsafe]] { _exit(rc); }
    return 0;
}

bool wait_for_server(int port) {
    int attempt = 0;
    while (attempt < 50) {
        int fd = -1;
        [[scpp::unsafe]] { fd = socket(2, 1, 0); }
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = static_cast<uint16_t>(2);
            [[scpp::unsafe]] {
                addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(2130706433));
                addr.sin_port = htons(static_cast<uint16_t>(port));
            }
            int i = 0;
            while (i < 8) {
                addr.sin_zero[i] = '\0';
                i = i + 1;
            }
            int rc = -1;
            [[scpp::unsafe]] {
                rc = connect(fd, static_cast<sockaddr*>(static_cast<void*>(&addr)), static_cast<unsigned int>(16));
                close(fd);
            }
            if (rc == 0) return true;
        }
        [[scpp::unsafe]] { usleep(static_cast<unsigned int>(50000)); }
        attempt = attempt + 1;
    }
    return false;
}

std::string send_request(int port, const char* request_cstr) {
    std::string request{request_cstr};
    int fd = -1;
    [[scpp::unsafe]] { fd = socket(2, 1, 0); }
    if (fd < 0) return std::string{};
    sockaddr_in addr{};
    addr.sin_family = static_cast<uint16_t>(2);
    [[scpp::unsafe]] {
        addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(2130706433));
        addr.sin_port = htons(static_cast<uint16_t>(port));
    }
    int i = 0;
    while (i < 8) {
        addr.sin_zero[i] = '\0';
        i = i + 1;
    }
    int connect_rc = -1;
    [[scpp::unsafe]] {
        connect_rc = connect(fd, static_cast<sockaddr*>(static_cast<void*>(&addr)), static_cast<unsigned int>(16));
    }
    if (connect_rc != 0) {
        [[scpp::unsafe]] { close(fd); }
        return std::string{};
    }
    size_t sent = 0;
    const char* req_ptr = request.c_str();
    while (sent < request.size()) {
        long n = 0;
        [[scpp::unsafe]] {
            n = send(fd, &req_ptr[sent], static_cast<unsigned long>(request.size() - sent), 0);
        }
        if (n <= 0) break;
        sent = sent + static_cast<size_t>(n);
    }
    std::string response{};
    char buffer[4096];
    while (true) {
        long n = 0;
        [[scpp::unsafe]] {
            n = recv(fd, &buffer[0], static_cast<unsigned long>(4096), 0);
        }
        if (n <= 0) break;
        std::string chunk{&buffer[0], static_cast<size_t>(n)};
        response.append(chunk);
    }
    [[scpp::unsafe]] { close(fd); }
    return response;
}

void run_integration_test(int& failures) {
    int self_pid = 0;
    [[scpp::unsafe]] { self_pid = getpid(); }
    std::string work_dir = "/tmp/scpp_httpserver_test_" + std::to_string(static_cast<int64_t>(self_pid));
    std::string root = prepare_docroot(work_dir);
    int port = reserve_port();
    int pid = start_server(root, port);
    expect(pid > 0, "server process spawned", failures);
    expect(wait_for_server(port), "server became reachable", failures);

    std::string get_root = send_request(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_root.starts_with("HTTP/1.1 200 OK"), "GET / returns 200", failures);
    expect(get_root.contains("index from scpp httpserver\n"), "GET / returns index body", failures);

    std::string get_file = send_request(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_file.contains("Content-Type: text/plain"), "GET /hello.txt has text/plain", failures);
    expect(get_file.contains("hello from file\n"), "GET /hello.txt returns body", failures);

    std::string get_css = send_request(port, "GET /assets/site.css HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_css.starts_with("HTTP/1.1 200 OK"), "GET /assets/site.css returns 200", failures);
    expect(get_css.contains("Content-Type: text/css"), "GET /assets/site.css has text/css", failures);
    expect(get_css.contains("font-family: sans-serif;"), "GET /assets/site.css returns CSS body", failures);

    std::string get_svg = send_request(port, "GET /assets/logo.svg HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_svg.starts_with("HTTP/1.1 200 OK"), "GET /assets/logo.svg returns 200", failures);
    expect(get_svg.contains("Content-Type: image/svg+xml"), "GET /assets/logo.svg has image/svg+xml", failures);
    expect(get_svg.contains("<svg"), "GET /assets/logo.svg returns SVG body", failures);

    std::string get_sitemap = send_request(port, "GET /sitemap.xml HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_sitemap.starts_with("HTTP/1.1 200 OK"), "GET /sitemap.xml returns 200", failures);
    expect(get_sitemap.contains("Content-Type: application/xml"), "GET /sitemap.xml has application/xml", failures);
    expect(get_sitemap.contains("<urlset"), "GET /sitemap.xml returns XML body", failures);

    std::string nested_index = send_request(port, "GET /book/en/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_index.starts_with("HTTP/1.1 200 OK"), "GET /book/en/ returns 200", failures);
    expect(nested_index.contains("Content-Type: text/html"), "GET /book/en/ has text/html", failures);
    expect(nested_index.contains("<title>SCPP book EN</title>"), "GET /book/en/ serves nested index", failures);

    std::string nested_index_without_slash = send_request(port, "GET /book/en HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_index_without_slash.starts_with("HTTP/1.1 200 OK"), "GET /book/en returns 200", failures);
    expect(nested_index_without_slash.contains("<h1>English book landing</h1>"),
           "GET /book/en serves directory index without trailing slash", failures);

    std::string nested_page = send_request(port, "GET /book/en/getting-started.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_page.starts_with("HTTP/1.1 200 OK"), "GET nested html page returns 200", failures);
    expect(nested_page.contains("Content-Type: text/html"), "GET nested html page has text/html", failures);
    expect(nested_page.contains("Getting started from generated site"),
           "GET nested html page returns nested body", failures);

    std::string head_file = send_request(port, "HEAD /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(head_file.starts_with("HTTP/1.1 200 OK"), "HEAD /hello.txt returns 200", failures);
    expect(!head_file.contains("hello from file\n"), "HEAD omits body", failures);

    std::string missing = send_request(port, "GET /missing.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(missing.starts_with("HTTP/1.1 404 Not Found"), "missing file returns 404", failures);

    std::string traversal = send_request(port, "GET /../secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(traversal.starts_with("HTTP/1.1 400 Bad Request"), "traversal returns 400", failures);

    std::string encoded_slash = send_request(port, "GET /nested%2fchild.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(encoded_slash.starts_with("HTTP/1.1 400 Bad Request"), "encoded slash returns 400", failures);

    std::string symlink_resp = send_request(port, "GET /link.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(symlink_resp.starts_with("HTTP/1.1 403 Forbidden") || symlink_resp.starts_with("HTTP/1.1 404 Not Found"),
           "symlink leaf is denied", failures);

    int status = 0;
    std::string cleanup_cmd = "rm -rf \"" + work_dir + "\"";
    [[scpp::unsafe]] {
        kill(pid, 15);
        waitpid(pid, &status, 0);
        system(cleanup_cmd.c_str());
    }
    return;
}

int main() {
    int failures = 0;
    run_integration_test(failures);
    if (failures != 0) {
        std::println("{} httpserver test(s) failed.", failures);
        return 1;
    }
    return 0;
}
