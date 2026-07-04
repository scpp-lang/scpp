int main() {
    char c = 'A';
    print_char(c);
    print_char('z');

    char buf[5];
    buf[0] = 'h';
    buf[1] = 'e';
    buf[2] = 'l';
    buf[3] = 'l';
    buf[4] = 'o';
    print_char(buf[0]);
    print_char(buf[4]);

    if (c == 'A') {
        print_int(1);
    } else {
        print_int(0);
    }
    return 0;
}
