#pragma once

extern "C" {

void* scpp_shared_ptr_count_new();
void scpp_shared_ptr_count_acquire(void* handle);
int scpp_shared_ptr_count_release(void* handle);
int scpp_shared_ptr_count_use_count(const void* handle);

}
