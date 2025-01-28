#include "quill-runtime.h"
#include <iostream>
#include <stdexcept>
#include <pthread.h>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdlib>

namespace quill {

static std::vector<WorkerDeque> worker_deques;
static std::vector<pthread_t> workers;
static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static int num_workers = 1; // Default number of workers

volatile boolean shutdown = false;
void init_quill_runtime() {
    const char* workers_env = std::getenv("QUILL_WORKERS");
    if (workers_env) {
        num_workers = std::stoi(workers_env);
    }
    worker_deques.resize(num_workers);
    workers.resize(num_workers);

    for (int i = 0; i < num_workers; ++i) {
        if (pthread_create(&workers[i], nullptr, worker_func, (void*)(intptr_t)i) != 0) {
            throw std::runtime_error("Failed to create worker thread");
        }
    }
}

void finalize_quill_runtime() {
    shutdown = true;
    for (int i = 0; i < num_workers; ++i) {
        pthread_join(workers[i], nullptr);
    }
}

WorkerDeque::WorkerDeque() : head(0), tail(0) {
    pthread_mutex_init(&lock, nullptr);
}

void WorkerDeque::push(std::function<void()> task) {
    pthread_mutex_lock(&lock);
    if (tail == tasks.size()) {
        throw std::runtime_error("Deque is full");
    }
    tasks.push_back(std::move(task));
    ++tail;
    pthread_mutex_unlock(&lock);
}

bool WorkerDeque::steal(std::function<void()> &task) {
    pthread_mutex_lock(&lock);
    if (head < tail) {
        task = std::move(tasks[head]);
        ++head;
        pthread_mutex_unlock(&lock);
        return true;
    }
    pthread_mutex_unlock(&lock);
    return false;
}

bool WorkerDeque::pop(std::function<void()> &task) {
    pthread_mutex_lock(&lock);
    if (head < tail) {
        task = std::move(tasks[--tail]);
        pthread_mutex_unlock(&lock);
        return true;
    }
    pthread_mutex_unlock(&lock);
    return false;
}

// Worker function for threads other than the main thread
void* worker_func(void* arg) {
    int worker_id = (intptr_t)arg;

    while (!shutdown) {
        find_and_execute_task(worker_id);
        // Optionally, add a small sleep to reduce contention
        usleep(100); 
    }

    return nullptr;
}

// Function to find and execute a task
void find_and_execute_task(int worker_id) {
    WorkerDeque& deque = worker_deques[worker_id];
    std::function<void()> task;

    // Try to pop a task from the local deque
    if (deque.pop(task)) {
        task();
        --finish_counter;
    } else {
        // Attempt to steal a task from other workers
        for (int i = 0; i < num_workers; ++i) {
            if (i != worker_id && worker_deques[i].steal(task)) {
                task();
                --finish_counter;
                return;
            }
        }
    }
}

int get_worker_id() {
    // Assuming unique worker ID is assigned through pthread
    return 0; // Placeholder for actual worker ID retrieval logic
}

// Implementations for async, start_finish, and end_finish
void async(std::function<void()> &&lambda) {
    int worker_id = get_worker_id();
    worker_deques[worker_id].push(std::move(lambda));
}
volatile int finish_counter = 0;
void start_finish() {
    // Handle the start of the finish scope (no recursion allowed)
    finish_counter = 0;
}

void end_finish() {
    int main_thread_id = 0; // Assuming the main thread has ID 0

    while (finish_counter != 0) {
        find_and_execute_task(main_thread_id);
    }
}

} // namespace quill
