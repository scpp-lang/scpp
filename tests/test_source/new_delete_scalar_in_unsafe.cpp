int main() {
    [[scpp::unsafe]] {
        int* p = new int(7);
        print_int(*p);
        delete p;
    }
    return 0;
}
