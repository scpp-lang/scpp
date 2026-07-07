// ch05 §5.15: a reference type (`T&`, an ordinary borrow) is never
// thread-movable, regardless of what it refers to -- moving a reference
// to another thread would not transfer the referent's ownership.
template<typename T>
void spawn(T& f [[scpp::thread_movable]]) {
    return;
}

int main() {
    int x = 5;
    spawn(x);
    return 0;
}
