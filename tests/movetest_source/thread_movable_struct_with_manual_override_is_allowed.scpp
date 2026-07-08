// ch05 §5.15: a manual `[[scpp::thread_movable]]` override on a struct's
// own declaration overrides the structural derivation entirely, even
// though the struct contains a raw pointer field (which the structural
// rule alone would always reject).
struct [[scpp::thread_movable]] RawBufferHandle {
    int* data;
    int len;
};

template<typename T>
void spawn(T&& f [[scpp::thread_movable]]) {
    return;
}

RawBufferHandle make_handle(int* d, int n) {
    RawBufferHandle h;
    h.data = d;
    h.len = n;
    return h;
}

int main() {
    int arr[3];
    arr[0] = 1;
    spawn(make_handle(arr, 3));
    return 0;
}
