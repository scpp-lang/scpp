#include <cstdlib>
#include <cstdint>
#include <random>

namespace {

std::mt19937* as_mt19937(void* handle) { return static_cast<std::mt19937*>(handle); }

[[noreturn]] void fail_fast() { std::abort(); }

} // namespace

extern "C" {

std::uint32_t scpp_random_device_next() {
    try {
        static std::random_device rd;
        return rd();
    } catch (...) {
        fail_fast();
    }
}

std::uint32_t scpp_random_device_min() {
    try {
        static std::random_device rd;
        return rd.min();
    } catch (...) {
        fail_fast();
    }
}

std::uint32_t scpp_random_device_max() {
    try {
        static std::random_device rd;
        return rd.max();
    } catch (...) {
        fail_fast();
    }
}

void* scpp_mt19937_new(std::uint32_t seed) {
    try {
        return new std::mt19937(seed);
    } catch (...) {
        fail_fast();
    }
}

void scpp_mt19937_delete(void* handle) {
    try {
        delete as_mt19937(handle);
    } catch (...) {
        fail_fast();
    }
}

std::uint32_t scpp_mt19937_next(void* handle) {
    try {
        return (*as_mt19937(handle))();
    } catch (...) {
        fail_fast();
    }
}

std::uint32_t scpp_mt19937_min() { return std::mt19937::min(); }

std::uint32_t scpp_mt19937_max() { return std::mt19937::max(); }

} // extern "C"
