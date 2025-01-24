#include <pthread.h>
#include <cstdlib>    // For malloc/free
#include <atomic>     // For atomic operations
#include <iostream>   // For logging (optional)

volatile bool shutdown = false;

void worker_routine() {
    while (!shutdown) {
        find_and_execute_task();
    }
}

void init_runtime() {
    int size = runtime_pool_size();
    for (int i = 1; i < size; i++) {
        pthread_create(worker_routine);
    }
}

volatile int finish_counter = 0;
void start_finish() {
    finish_counter = 0; //reset
}


void async(task) {
    lock_finish();
    finish_counter++;//concurrent access
    unlock_finish();
    // copy task on heap
    void* p = malloc(task_size);
    memcpy(p, task, task_size);
    //thread-safe push_task_to_runtime
    push_task_to_runtime(&p);
    return;
}