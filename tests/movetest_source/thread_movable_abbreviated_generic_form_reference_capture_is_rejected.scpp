// ch05 §5.15: the thread-safety constraint check applies equally to the
// abbreviated generic-function form (`Concept auto&&`, ch05 §5.11), not
// just the full `template<typename T>` header form -- a separate call-
// site monomorphization path that must independently wire in the same
// check.
template<typename T>
concept Runnable = requires(T t) { t(); };

void spawn(Runnable auto&& f [[scpp::thread_movable]]) {
    return;
}

int main() {
    int x = 5;
    spawn([&x]() -> int { return x; });
    return 0;
}
