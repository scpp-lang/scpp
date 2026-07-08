// ch05 §5.15: every scalar type is unconditionally thread-movable, so a
// plain `int` argument satisfies a `[[scpp::thread_movable]]`-constrained
// rvalue-reference parameter.
template<typename T>
void spawn(T&& f [[scpp::thread_movable]]) {
    return;
}

int main() {
    spawn(5);
    return 0;
}
