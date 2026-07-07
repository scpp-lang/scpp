// ch05 §5.15: a closure with a by-*mutable*-reference capture is never
// thread-shareable -- two threads concurrently calling through a shared
// mutable reference could race, even though alias-XOR-mutability already
// guarantees no other *borrow* of the referent exists locally.
template<typename T>
void broadcast(const T& f [[scpp::thread_shareable]]) {
    return;
}

int main() {
    int x = 5;
    broadcast([&x]() -> int { return x; });
    return 0;
}
