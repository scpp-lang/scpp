// ch06 §6: `static_cast<T>(expr)`/`(T)expr` are the only way to convert
// between two distinct scpp scalar types -- both spellings work, for
// every (int/float/bool/char) pairing: widening/narrowing integers,
// float<->int, and same-width signed/unsigned reinterpretation.
int main() {
    int64_t big = 4294967297;
    int32_t truncated = (int32_t)big;

    double d = 3.9;
    int i = static_cast<int>(d);

    int x = 5;
    double y = static_cast<double>(x);

    uint32_t u = 4294967295;
    int32_t s = (int32_t)u;

    bool b = true;
    int bi = (int)b;

    char c = 'A';
    int ci = static_cast<int>(c);

    int y_check = 0;
    if (y > 4.0) { y_check = 1; }

    return truncated + i + bi + ci + s + y_check;
}
