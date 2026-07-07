// ch05 §5.15: `std::unique_ptr<T>` is thread-movable iff `T` itself is
// (here, `int`, always thread-movable) -- unique ownership means handing
// the whole `unique_ptr` to another thread transfers exclusive access to
// the pointee along with it.
template<typename T>
void spawn(T f [[scpp::thread_movable]]) {
    return;
}

int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    spawn(std::move(p));
    return 0;
}
