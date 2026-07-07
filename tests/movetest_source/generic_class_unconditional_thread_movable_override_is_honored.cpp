template<typename T>
class [[scpp::thread_movable]] MyOwningBox {
public:
    T* ptr_;
};

template<typename T>
void spawn(T&& f [[scpp::thread_movable]]) {
    return;
}

MyOwningBox<int> make_box(int* ptr) {
    MyOwningBox<int> box;
    box.ptr_ = ptr;
    return box;
}

int main() {
    int x = 1;
    spawn(make_box(&x));
    return 0;
}
