#include "scpp_shared_ptr_wrapper.h"

#include <atomic>
#include <cstdlib>

namespace {

using SharedCount = std::atomic<int>;

SharedCount* as_shared_count(void* handle) { return static_cast<SharedCount*>(handle); }
const SharedCount* as_shared_count(const void* handle) { return static_cast<const SharedCount*>(handle); }

[[noreturn]] void fail_fast() { std::abort(); }

} // namespace

extern "C" {

void* scpp_shared_ptr_count_new() {
    try {
        return new SharedCount(1);
    } catch (...) {
        fail_fast();
    }
}

void scpp_shared_ptr_count_acquire(void* handle) {
    try {
        SharedCount* count = as_shared_count(handle);
        if (count == nullptr) fail_fast();
        count->fetch_add(1, std::memory_order_acq_rel);
    } catch (...) {
        fail_fast();
    }
}

int scpp_shared_ptr_count_release(void* handle) {
    try {
        SharedCount* count = as_shared_count(handle);
        if (count == nullptr) fail_fast();
        int old = count->fetch_sub(1, std::memory_order_acq_rel);
        if (old <= 0) fail_fast();
        if (old == 1) {
            delete count;
            return 1;
        }
        return 0;
    } catch (...) {
        fail_fast();
    }
}

int scpp_shared_ptr_count_use_count(const void* handle) {
    try {
        const SharedCount* count = as_shared_count(handle);
        if (count == nullptr) return 0;
        return count->load(std::memory_order_acquire);
    } catch (...) {
        fail_fast();
    }
}

} // extern "C"
