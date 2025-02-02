#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>

namespace quill {

template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<std::unique_ptr<std::function<void()>>, DEQUE_SIZE> tasks;  // Array of task pointers
    int head;   // Index for the top (head) - accessed by thieves in FIFO order
    int tail;   // Index for the bottom (tail) - private to the worker, LIFO order
    pthread_mutex_t lock; // Mutex for thread-safe operations

    WorkerDeque();
    void push(std::function<void()> task);  // Push task for the worker (LIFO)
    bool steal(std::function<void()> &task);  // Steal task from another worker (FIFO)
    bool pop(std::function<void()> &task);  // Pop task from worker's own deque (LIFO)
};

    extern int num_workers;                      // Number of worker threads
    extern std::vector<pthread_t> workers;       // Vector to hold thread handles
    extern pthread_t master_thread;              // Master thread handle
    
    void worker_func(void* arg);
    void* thread_func(void *args);

} // namespace quill

#endif // QUILL_RUNTIME_H