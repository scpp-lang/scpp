import std;

struct RawPointerHolder {
    int* ptr;
};

template<typename T>
void spawn(T f [[scpp::thread_movable]]) {
    return;
}

RawPointerHolder make_holder(int* ptr) {
    RawPointerHolder holder;
    holder.ptr = ptr;
    return holder;
}

int main() {
    int x = 1;
    std::unique_ptr<RawPointerHolder> holder = std::make_unique<RawPointerHolder>(make_holder(&x));
    spawn(std::move(holder));
    return 0;
}
