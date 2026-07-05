// ch02 §2.1: extern "C" block form (sugar for repeating `extern "C"` on
// each declaration) declaring real libc allocator functions; `void*`
// converts implicitly to another pointer type on initialization.
extern "C" {
    void* malloc(int size);
    void free(void* p);
}

safe int use_heap() {
    int result = 0;
    unsafe {
        void* raw = malloc(4);
        int* typed = raw;
        *typed = 77;
        result = *typed;
        free(raw);
    }
    return result;
}

int main() {
    return use_heap();
}
