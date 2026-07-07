// ch06 §6: the full numeric family (int8_t/16/32/64_t, uint8/16/32/64_t,
// long, unsigned int/long, float/double/float32_t/float64_t, size_t,
// ptrdiff_t) all parse as type names and support basic arithmetic. A
// bare integer/float literal adapts directly to whatever scalar type
// it's initializing/passed to/returned as (ch06: this is literal type
// inference, not an implicit conversion of an already-typed value).
int8_t s8(int8_t x) { return x; }
uint8_t u8(uint8_t x) { return x; }
int16_t s16(int16_t x) { return x; }
uint16_t u16(uint16_t x) { return x; }
int32_t s32(int32_t x) { return x; }
uint32_t u32(uint32_t x) { return x; }
int64_t s64(int64_t x) { return x; }
uint64_t u64(uint64_t x) { return x; }
long l(long x) { return x; }
unsigned int ui(unsigned int x) { return x; }
unsigned long ul(unsigned long x) { return x; }
float f32(float x) { return x; }
double f64(double x) { return x; }
float32_t f32t(float32_t x) { return x; }
float64_t f64t(float64_t x) { return x; }
size_t sz(size_t x) { return x; }
ptrdiff_t pd(ptrdiff_t x) { return x; }

int main() {
    int8_t a = 1;
    uint8_t b = 2;
    int64_t c = 1000000000000;
    unsigned int d = 4000000000;
    float e = 1.5;
    double f = 2.5;
    size_t g = 100;
    ptrdiff_t h = -100;

    // Basic arithmetic + comparisons on a few representative widths.
    int64_t sum = c + 1;
    unsigned int usum = d + 1;
    double fsum = f + 2.5;
    bool cmp = d > 10;

    // Round-trip every type through its own identity function.
    s8(a);
    u8(b);
    s64(c);
    ui(d);
    f32(e);
    f64(f);
    sz(g);
    pd(h);

    return 0;
}
