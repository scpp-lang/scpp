// ch05 §5.3/ch06 backlog: "Implementation of the
// [[scpp::lifetime(name)]] multi-group mechanism ... design only so far."
safe const int& get_x(const int& x [[scpp::lifetime(a)]], const int& y [[scpp::lifetime(b)]]) [[scpp::lifetime(a)]] {
    return x;
}

int main() {
    int a = 1;
    int b = 2;
    return get_x(a, b);
}
