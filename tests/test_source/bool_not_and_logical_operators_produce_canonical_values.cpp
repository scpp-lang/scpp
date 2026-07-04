int main() {
    bool t = true;
    bool f = !t;
    if (f == false) {
        print_int(1);
    } else {
        print_int(0);
    }

    bool a = false;
    bool b = !a;
    if (b == true) {
        print_int(2);
    } else {
        print_int(0);
    }

    print_bool(!t);
    print_bool(!a);

    bool c1 = true && false;
    bool c2 = true || false;
    print_bool(c1);
    print_bool(c2);

    return 0;
}
