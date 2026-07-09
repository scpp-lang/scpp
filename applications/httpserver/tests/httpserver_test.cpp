#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef SCPP_BINARY_PATH
#error "SCPP_BINARY_PATH must be defined"
#endif
#ifndef SCPP_HTTPSERVER_MODULE_PATH
#error "SCPP_HTTPSERVER_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STD_MODULE_PATH
#error "SCPP_STDLIB_STD_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STD_STRING_MODULE_PATH
#error "SCPP_STDLIB_STD_STRING_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STD_MEMORY_MODULE_PATH
#error "SCPP_STDLIB_STD_MEMORY_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH
#error "SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STD_THREAD_MODULE_PATH
#error "SCPP_STDLIB_STD_THREAD_MODULE_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_STRING_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_STRING_WRAPPER_LIB_PATH must be defined"
#endif
#ifndef SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH must be defined"
#endif
#ifndef SCPP_HTTPSERVER_TESTDATA_DIR
#error "SCPP_HTTPSERVER_TESTDATA_DIR must be defined"
#endif
#ifndef SCPP_HTTPSERVER_WORK_DIR
#error "SCPP_HTTPSERVER_WORK_DIR must be defined"
#endif

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        ++failures;
    }
}

int reserve_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 19080;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close(fd);
        return 19080;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

std::string escape_c_string(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::filesystem::path prepare_docroot() {
    std::filesystem::path work(SCPP_HTTPSERVER_WORK_DIR);
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work);
    std::filesystem::copy(SCPP_HTTPSERVER_TESTDATA_DIR, work / "docroot",
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::ofstream(work / "docroot" / "secret.txt") << "secret\n";
    std::error_code ec;
    std::filesystem::create_symlink(work / "docroot" / "hello.txt", work / "docroot" / "link.txt", ec);
    return work / "docroot";
}

std::filesystem::path build_server_binary(const std::filesystem::path& root, int port, bool enable_default_charset) {
    std::filesystem::path work(SCPP_HTTPSERVER_WORK_DIR);
    std::filesystem::create_directories(work);
    std::filesystem::path source = work / "generated_main.scpp";
    std::filesystem::path binary = work / "httpserver_test_bin";
    std::ofstream out(source);
    out << "import httpserver;\n\n"
        << "int main() {\n"
        << "    httpserver::ServerBuilder builder;\n"
        << "    builder.mount(\"/\", \"" << escape_c_string(root.string()) << "\");\n"
        << "    builder.set_port(" << port << ");\n"
        << "    builder.set_max_connections(32);\n"
        << "    builder.set_worker_count(2);\n"
        << (enable_default_charset ? "    builder.set_default_charset(\"utf-8\");\n" : "")
        << "    builder.allow_index_html(true);\n"
        << "    builder.deny_hidden_files(true);\n"
        << "    return builder.serve();\n"
        << "}\n";
    out.close();

    std::ostringstream cmd;
    cmd << SCPP_BINARY_PATH << " build " << source.string()
        << " -o " << binary.string()
        << " --import httpserver=" << SCPP_HTTPSERVER_MODULE_PATH
        << " --import std=" << SCPP_STDLIB_STD_MODULE_PATH
        << " --import std:string=" << SCPP_STDLIB_STD_STRING_MODULE_PATH
        << " --import std:memory=" << SCPP_STDLIB_STD_MEMORY_MODULE_PATH
        << " --import std:functional=" << SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH
        << " --import std:thread=" << SCPP_STDLIB_STD_THREAD_MODULE_PATH
        << " --link " << SCPP_STDLIB_STRING_WRAPPER_LIB_PATH
        << " --link " << SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH;
    int rc = std::system(cmd.str().c_str());
    expect(rc == 0, "generated httpserver app compiles");
    return binary;
}

pid_t start_server(const std::filesystem::path& binary) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    execl(binary.c_str(), binary.c_str(), nullptr);
    _exit(127);
}

bool wait_for_server(int port) {
    for (int i = 0; i < 50; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(fd);
            return true;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::string send_request(int port, const std::string& request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return {};
    }
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[4096];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return response;
}

void expect_content_type(const std::string& response, const std::string& media_type, bool expect_charset,
                         const std::string& message) {
    std::string expected_header = "Content-Type: " + media_type;
    if (expect_charset) {
        expected_header += "; charset=utf-8";
        expect(response.find(expected_header) != std::string::npos, message + " with charset");
        return;
    }
    expected_header += "\r\n";
    expect(response.find(expected_header) != std::string::npos, message + " without charset");
    expect(response.find("Content-Type: " + media_type + "; charset=") == std::string::npos,
           message + " should not include a charset");
}

void stop_server(pid_t pid) {
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
}

void run_server_case(const std::filesystem::path& root, int port, bool expect_charset) {
    const auto binary = build_server_binary(root, port, expect_charset);
    pid_t pid = start_server(binary);
    expect(pid > 0, "server process spawned");
    expect(wait_for_server(port), "server became reachable");

    std::string get_root = send_request(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_root.find("HTTP/1.1 200 OK") == 0, "GET / returns 200");
    expect_content_type(get_root, "text/html", expect_charset, "GET / has text/html");
    expect(get_root.find("index from scpp httpserver\n") != std::string::npos, "GET / returns index body");

    std::string get_file = send_request(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect_content_type(get_file, "text/plain", expect_charset, "GET /hello.txt has text/plain");
    expect(get_file.find("hello from file\n") != std::string::npos, "GET /hello.txt returns body");

    std::string get_css = send_request(port, "GET /assets/site.css HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(get_css.find("HTTP/1.1 200 OK") == 0, "GET /assets/site.css returns 200");
    expect_content_type(get_css, "text/css", expect_charset, "GET /assets/site.css has text/css");
    expect(get_css.find("font-family: sans-serif;") != std::string::npos, "GET /assets/site.css returns CSS body");

    std::string nested_index = send_request(port, "GET /book/en/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_index.find("HTTP/1.1 200 OK") == 0, "GET /book/en/ returns 200");
    expect_content_type(nested_index, "text/html", expect_charset, "GET /book/en/ has text/html");
    expect(nested_index.find("<title>SCPP book EN</title>") != std::string::npos, "GET /book/en/ serves nested index");

    std::string nested_index_without_slash = send_request(port, "GET /book/en HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_index_without_slash.find("HTTP/1.1 200 OK") == 0, "GET /book/en returns 200");
    expect(nested_index_without_slash.find("<h1>English book landing</h1>") != std::string::npos,
           "GET /book/en serves directory index without trailing slash");

    std::string nested_page = send_request(port, "GET /book/en/getting-started.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(nested_page.find("HTTP/1.1 200 OK") == 0, "GET nested html page returns 200");
    expect_content_type(nested_page, "text/html", expect_charset, "GET nested html page has text/html");
    expect(nested_page.find("Getting started from generated site") != std::string::npos,
           "GET nested html page returns nested body");

    std::string head_file = send_request(port, "HEAD /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(head_file.find("HTTP/1.1 200 OK") == 0, "HEAD /hello.txt returns 200");
    expect(head_file.find("hello from file\n") == std::string::npos, "HEAD omits body");

    std::string missing = send_request(port, "GET /missing.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(missing.find("HTTP/1.1 404 Not Found") == 0, "missing file returns 404");

    std::string traversal = send_request(port, "GET /../secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(traversal.find("HTTP/1.1 400 Bad Request") == 0, "traversal returns 400");

    std::string encoded_slash = send_request(port, "GET /nested%2fchild.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(encoded_slash.find("HTTP/1.1 400 Bad Request") == 0, "encoded slash returns 400");

    std::string symlink = send_request(port, "GET /link.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(symlink.find("HTTP/1.1 403 Forbidden") == 0 || symlink.find("HTTP/1.1 404 Not Found") == 0,
           "symlink leaf is denied");

    stop_server(pid);
}

void run_integration_test() {
    const auto root = prepare_docroot();
    run_server_case(root, reserve_port(), false);
    run_server_case(root, reserve_port(), true);
}
} // namespace

int main() {
    run_integration_test();
    if (failures != 0) {
        std::cerr << failures << " httpserver test(s) failed.\n";
        return 1;
    }
    return 0;
}
