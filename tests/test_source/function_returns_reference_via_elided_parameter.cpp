int& identity(int& a) {
    return a;
}

int main() {
    int x = 41;
    int y = identity(x);
    print_int(y);
    return 0;
}
