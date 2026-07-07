// ch05 §5.15: every scalar type is unconditionally thread-shareable, so
// a plain `int` argument satisfies a `[[scpp::thread_shareable]]`-
// constrained `const T&` parameter.
template<typename T>
void broadcast(const T& x [[scpp::thread_shareable]]) {
    return;
}

int main() {
    int n = 5;
    broadcast(n);
    return 0;
}
