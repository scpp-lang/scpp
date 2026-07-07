import std;

int main() {
    print_bool(scpp::is_thread_movable(int));
    print_bool(scpp::is_thread_shareable(int));
    print_bool(scpp::is_thread_movable(std::unique_ptr<int>));
    print_bool(scpp::is_thread_shareable(std::unique_ptr<int>));
    return 0;
}
