template<typename T>
int add_one(T x) {
    return x + 1;
}

int main() {
    int (*fp)(int) = add_one<int>;
    print_int(fp(7));
    return 0;
}
