#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>

namespace quill {

// Data structure to hold the deque and task management
struct WorkerDeque {
    std::vector<std::function<void()>> tasks;
    int head;
    int tail;
    pthread_mutex_t lock;

    WorkerDeque();
    void push(std::function<void()> task);
    bool steal(std::function<void()> &task);
    bool pop(std::function<void()> &task);
};

// Initialize the Quill runtime
void init_quill_runtime();

// Finalize the Quill runtime
void finalize_quill_runtime();

// Worker function that runs in a separate thread
void* worker_func(void* arg);

// Helper function to get worker ID using pthread
int get_worker_id();

} // namespace quill

#endif // QUILL_RUNTIME_H
