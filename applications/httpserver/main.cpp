import httpserver;

int main() {
    httpserver::ServerBuilder builder{};
    builder.mount("/", ".");
    builder.set_port(8080);
    builder.set_max_connections(32);
    builder.set_worker_count(4);
    builder.allow_index_html(true);
    builder.deny_hidden_files(true);
    return builder.serve();
}
