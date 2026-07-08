// ch05 §5.15: a closure with any by-reference capture at all is never
// thread-movable -- moving it to another thread does not transfer the
// referent's ownership, so the referent would remain reachable (and thus
// unsound to alias) from the original thread.
template<typename T>
void spawn(T&& f [[scpp::thread_movable]]) {
    return;
}

int main() {
    int x = 5;
    spawn([&x]() -> int { return x; });
    return 0;
}
