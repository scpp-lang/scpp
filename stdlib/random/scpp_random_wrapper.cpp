#include <cstdlib>
#include <cstdint>
#include <limits>
#include <random>

namespace {

std::mt19937* as_mt19937(void* handle) { return static_cast<std::mt19937*>(handle); }
std::uniform_int_distribution<int>* as_uniform_int_distribution_int(void* handle) {
    return static_cast<std::uniform_int_distribution<int>*>(handle);
}

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

void* scpp_random_uniform_int_new(int minimum, int maximum) {
    try {
        if (minimum > maximum) fail_fast();
        return new std::uniform_int_distribution<int>(minimum, maximum);
    } catch (...) {
        fail_fast();
    }
}

void scpp_random_uniform_int_delete(void* handle) {
    try {
        delete as_uniform_int_distribution_int(handle);
    } catch (...) {
        fail_fast();
    }
}

int scpp_random_uniform_int_next(void* handle, void* generator_handle) {
    try {
        return (*as_uniform_int_distribution_int(handle))(*as_mt19937(generator_handle));
    } catch (...) {
        fail_fast();
    }
}

int scpp_random_uniform_int_min(void* handle) {
    try {
        return as_uniform_int_distribution_int(handle)->min();
    } catch (...) {
        fail_fast();
    }
}

int scpp_random_uniform_int_max(void* handle) {
    try {
        return as_uniform_int_distribution_int(handle)->max();
    } catch (...) {
        fail_fast();
    }
}

} // extern "C"
