int make_value() {
    return 99;
}
int take_rref(int&& x) {
    return x;
}
int main() {
    print_int(take_rref(make_value()));
    print_int(take_rref(1));
    return 0;
}
