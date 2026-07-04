extern "C" {
    void* malloc(int size);
    void free(void* p);
}

safe void store_and_read() {
    unsafe {
        void* raw = malloc(4);
        int* typed = raw;
        *typed = 99;
        print_int(*typed);
        free(raw);
        return;
    }
}

int main() {
    store_and_read();
    return 0;
}
