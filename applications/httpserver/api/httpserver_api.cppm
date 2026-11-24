export module httpserver:api;

import std;
import :runtime;

namespace httpserver {

export class ServerBuilder {
    public:
        virtual ~ServerBuilder() { return; }
private:
    std::string root_{};
    int port_{};
    int max_connections_{};
    int worker_count_{};
    bool allow_index_html_{};
    bool deny_hidden_files_{};
    bool has_error_{};

    bool assign_root(std::string filesystem_root) {
        if (filesystem_root.empty()) {
            return false;
        }
        this->root_ = std::move(filesystem_root);
        return true;
    }

public:
    ServerBuilder() {
        this->root_ = std::string{"."};
        this->port_ = 18080;
        this->max_connections_ = 32;
        this->worker_count_ = 1;
        this->allow_index_html_ = true;
        this->deny_hidden_files_ = true;
        this->has_error_ = false;
        return;
    }

    void set_port(int port) {
        if (port <= 0) {
            this->has_error_ = true;
            return;
        }
        this->port_ = port;
        return;
    }

    void set_max_connections(int max_connections) {
        if (max_connections <= 0 || max_connections > 64) {
            this->has_error_ = true;
            return;
        }
        this->max_connections_ = max_connections;
        return;
    }

    void set_worker_count(int worker_count) {
        if (worker_count <= 0 || worker_count > 8) {
            this->has_error_ = true;
            return;
        }
        this->worker_count_ = worker_count;
        return;
    }

    void mount(std::string url_prefix, std::string filesystem_root) {
        if (url_prefix != "/") {
            this->has_error_ = true;
            return;
        }
        if (!this->assign_root(std::move(filesystem_root))) {
            this->has_error_ = true;
        }
        return;
    }

    void allow_index_html(bool allow) {
        this->allow_index_html_ = allow;
        return;
    }

    void deny_hidden_files(bool deny) {
        this->deny_hidden_files_ = deny;
        return;
    }

    int serve() {
        if (this->has_error_) {
            return 1;
        }
        if (this->root_.empty()) {
            return 1;
        }
        if (this->port_ <= 0) {
            return 1;
        }
        if (this->max_connections_ <= 0 || this->max_connections_ > 64) {
            return 1;
        }
        if (this->worker_count_ <= 0 || this->worker_count_ > 8) {
            return 1;
        }
        return run_server(this->root_, this->port_, this->max_connections_, this->allow_index_html_,
                          this->deny_hidden_files_, this->worker_count_);
    }
};

} // namespace httpserver
