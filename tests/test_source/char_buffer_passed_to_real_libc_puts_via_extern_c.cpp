extern "C" int puts(const char* s);

int main() {
    char msg[6];
    msg[0] = 'h';
    msg[1] = 'e';
    msg[2] = 'l';
    msg[3] = 'l';
    msg[4] = 'o';
    msg[5] = '\0';
    unsafe {
        puts(msg);
    }
    return 0;
}
