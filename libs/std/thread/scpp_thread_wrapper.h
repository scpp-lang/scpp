#ifndef SCPP_THREAD_WRAPPER_H
#define SCPP_THREAD_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

void* scpp_thread_spawn(void (*trampoline)(void*), void* arg);
void scpp_thread_join_and_delete(void* handle);
void scpp_thread_detach_and_delete(void* handle);

#ifdef __cplusplus
}
#endif

#endif // SCPP_THREAD_WRAPPER_H
