export module std;

namespace std {
    export safe int f(int x) {
        return std::g(x);
    }
    safe int g(int x) {
        return x + 1;
    }
}
