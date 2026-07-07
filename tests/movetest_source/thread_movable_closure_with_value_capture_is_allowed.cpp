// ch05 §5.15: a closure with no by-reference capture at all, and whose
// every by-value-captured member is itself thread-movable (here, a
// plain `int`), is itself thread-movable -- a closure literal is just a
// synthesized class, so the ordinary struct/class field-recursion rule
// covers it for free.
template<typename T>
void spawn(T&& f [[scpp::thread_movable]]) {
    return;
}

int main() {
    int x = 5;
    spawn([x]() -> int { return x; });
    return 0;
}
