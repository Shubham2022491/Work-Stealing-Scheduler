#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <pthread.h>
using namespace std;

namespace quill {

    int num_workers = 1; // Default to 1 worker, including master thread
    std::vector<std::vector<std::function<void()>>> worker_deques;
    std::vector<pthread_t> workers;
    pthread_t master_thread;  // This is the main (master) thread


    volatile bool shutdown = false;
    void init_runtime() {
        const char* workers_env = std::getenv("QUILL_WORKERS");
        if (workers_env) {
            num_workers = std::stoi(workers_env);
        }
        if (num_workers < 1) {
            num_workers = 1; // Ensure at least 1 worker
        }
        worker_deques.resize(num_workers);
        workers.resize(num_workers);
        master_thread = pthread_self(); // Main thread as master

        for (int i = 1; i < num_workers; ++i) {
            if (pthread_create(&workers[i], nullptr, (void*(*)(void*))worker_func, (void*)(intptr_t)i) != 0) {
                throw std::runtime_error("Failed to create worker thread");
            }
            std::cout<<"Worker "<<i<<" created"<<std::endl;
        }
        std::cout << "Quill runtime initialized with " << num_workers << " threads." << std::endl;
    }

    void find_and_execute_task(int worker_id) {
        cout << "Worker " << worker_id << " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        // std::function<void()> task;

        // // Try to pop a task from the local deque
        // if (deque.pop(task)) {
        //     task();
        //     --finish_counter;
        // } 
        // else {
        //     // Attempt to steal a task from other workers
        //     for (int i = 0; i < num_workers; ++i) {
        //         if (i != worker_id && worker_deques[i].steal(task)) {
        //             task();
        //             --finish_counter;
        //             return;
        //         }
        //     }
        // }
    }

    void worker_func(void* arg) {
        int worker_id = (intptr_t)arg;
        std::cout << "Worker " << worker_id << " started" << std::endl;
        cout<<shutdown<<endl;
        while (!shutdown) {
            find_and_execute_task(worker_id);
            // Optionally, add a small sleep to reduce contention
            usleep(100); 
        }
    }

    
}

//     // Finalize the Quill runtime
//     void finalize_quill_runtime() {
//         shutdown = true;
//         for (int i = 0; i < num_workers; ++i) {
//             pthread_join(workers[i], nullptr);  // Join all worker threads
//         }
//     }


// // WorkerDeque constructor initializes the deque
// template <size_t DEQUE_SIZE>
// quill::WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
//     pthread_mutex_init(&lock, nullptr);
// }

// // WorkerDeque destructor cleans up mutex resources
// template <size_t DEQUE_SIZE>
// quill::WorkerDeque<DEQUE_SIZE>::~WorkerDeque() {
//     pthread_mutex_destroy(&lock);
// }

// // Push a task into the deque (private to the worker, LIFO)
// template <size_t DEQUE_SIZE>
// void quill::WorkerDeque<DEQUE_SIZE>::push(std::function<void()> task) {
//     pthread_mutex_lock(&lock);
//     int next_tail = (tail + 1) % DEQUE_SIZE;  // Circular buffer logic
//     if (next_tail != head) {  // Check if the deque is not full
//         tasks[tail] = std::move(task);
//         tail = next_tail;
//     } else {
//         // Handle overflow: optional logging or error handling
//         std::cerr << "Error: Worker deque overflow\n";
//     }
//     pthread_mutex_unlock(&lock);
// }

// // Steal a task from the top (head) of the deque (FIFO, for other workers)
// template <size_t DEQUE_SIZE>
// bool quill::WorkerDeque<DEQUE_SIZE>::steal(std::function<void()> &task) {
//     pthread_mutex_lock(&lock);
//     if (head != tail) {  // Check if the deque is not empty
//         int steal_index = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE;  // Circular buffer logic
//         task = std::move(tasks[steal_index]);
//         tail = steal_index;  // Update the tail after stealing
//         pthread_mutex_unlock(&lock);
//         return true;
//     }
//     pthread_mutex_unlock(&lock);
//     return false;
// }

// // Pop a task from the bottom (tail) of the deque (private to the worker, LIFO)
// template <size_t DEQUE_SIZE>
// bool quill::WorkerDeque<DEQUE_SIZE>::pop(std::function<void()> &task) {
//     pthread_mutex_lock(&lock);
//     if (head != tail) {  // Check if the deque is not empty
//         task = std::move(tasks[head]);
//         head = (head + 1) % DEQUE_SIZE;  // Circular buffer logic
//         pthread_mutex_unlock(&lock);
//         return true;
//     }
//     pthread_mutex_unlock(&lock);
//     return false;
// }

// // Worker function for threads other than the main thread
// void* worker_func(void* arg) {
//     int worker_id = (intptr_t)arg;

//     while (!shutdown) {
//         find_and_execute_task(worker_id);
//         // Optionally, add a small sleep to reduce contention
//         usleep(100); 
//     }

//     return nullptr;
// }

// // Function to find and execute a task
// void find_and_execute_task(int worker_id) {
//     WorkerDeque& deque = worker_deques[worker_id];
//     std::function<void()> task;

//     // Try to pop a task from the local deque
//     if (deque.pop(task)) {
//         task();
//         --finish_counter;
//     } else {
//         // Attempt to steal a task from other workers
//         for (int i = 0; i < num_workers; ++i) {
//             if (i != worker_id && worker_deques[i].steal(task)) {
//                 task();
//                 --finish_counter;
//                 return;
//             }
//         }
//     }
// }

// int get_worker_id() {
//     // Assuming unique worker ID is assigned through pthread
//     return 0; // Placeholder for actual worker ID retrieval logic
// }

// // Implementations for async, start_finish, and end_finish
// void async(std::function<void()> &&lambda) {
//     int worker_id = get_worker_id();
//     worker_deques[worker_id].push(std::move(lambda));
// }


// volatile int finish_counter = 0;
// void start_finish() {
//     // Handle the start of the finish scope (no recursion allowed)
//     finish_counter = 0;
// }

// void end_finish() {
//     int main_thread_id = 0; // Assuming the main thread has ID 0

//     while (finish_counter != 0) {
//         find_and_execute_task(main_thread_id);
//     }
// }

// } // namespace quill
