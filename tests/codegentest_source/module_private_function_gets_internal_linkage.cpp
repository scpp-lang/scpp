export module std;

namespace std {
    export int f(int x) {
        return std::g(x);
    }
    int g(int x) {
        return x + 1;
    }
}
