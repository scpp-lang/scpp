extern "C" int puts(const char* s);

void print_buffer(char* msg) {
    [[scpp::unsafe]] {
        puts(msg);
    }
    return;
}

int main() {
    char msg[6];
    msg[0] = 'h';
    msg[1] = 'e';
    msg[2] = 'l';
    msg[3] = 'l';
    msg[4] = 'o';
    msg[5] = '\0';
    print_buffer(msg);
    return 0;
}
