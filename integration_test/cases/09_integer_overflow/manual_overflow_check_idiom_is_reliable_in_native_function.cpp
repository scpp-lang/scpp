// ch05 §5.8: "Manual overflow detection becomes reliable inside unsafe
// { }: the classic if (x + 1 < x) idiom is unreliable for signed x in
// real C++ ... Since scpp never emits nsw, there is no such license to
// exploit, so this idiom works exactly as its arithmetic reads: x =
// INT_MAX wraps x + 1 to INT_MIN, and INT_MIN < INT_MAX is (correctly)
// true." Tested here in a native function (unannotated -- already
// "unsafe" everywhere, so the wraparound guarantee already applies with
// no unsafe { } needed).
int main() {
    int x = 2147483647;
    int y = x + 1;
    if (y < x) {
        return 1;
    }
    return 0;
}
