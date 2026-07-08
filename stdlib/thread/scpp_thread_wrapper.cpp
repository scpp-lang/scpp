#include "scpp_thread_wrapper.h"

#include <cstdlib>
#include <thread>

namespace {

std::thread* as_thread(void* handle) { return static_cast<std::thread*>(handle); }

[[noreturn]] void fail_fast() { std::abort(); }

} // namespace

extern "C" {

void* scpp_thread_spawn(void (*trampoline)(void*), void* arg) {
    try {
        return new std::thread(trampoline, arg);
    } catch (...) {
        fail_fast();
    }
}

void scpp_thread_join_and_delete(void* handle) {
    try {
        std::thread* thread = as_thread(handle);
        if (thread == nullptr || !thread->joinable()) fail_fast();
        thread->join();
        delete thread;
    } catch (...) {
        fail_fast();
    }
}

void scpp_thread_detach_and_delete(void* handle) {
    try {
        std::thread* thread = as_thread(handle);
        if (thread == nullptr || !thread->joinable()) fail_fast();
        thread->detach();
        delete thread;
    } catch (...) {
        fail_fast();
    }
}

} // extern "C"
