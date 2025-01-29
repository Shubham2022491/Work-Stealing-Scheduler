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
    constexpr size_t DEQUE_SIZE = 100;  // Example size for all worker deques
    // Define a fixed-size deque using std::array
    
    pthread_t master_thread;  // This is the main (master) thread
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;


    // Constructor to initialize the deque
    template <size_t DEQUE_SIZE>
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
        pthread_mutex_init(&lock, nullptr);  // Initialize the mutex
    }

    
    // Push task to the worker's deque (LIFO order)
    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::push(std::function<void()> task) {
        // pthread_mutex_lock(&lock);  // Lock to ensure thread safety

        if (tail < DEQUE_SIZE) {
            tasks[tail] = std::make_unique<std::function<void()>>(std::move(task));  // Create task on heap
            tail++;
        }

        // pthread_mutex_unlock(&lock);  // Unlock after modification
    }

    // Steal task from the worker's deque (FIFO order)
    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(std::function<void()> &task) {
        pthread_mutex_lock(&lock);  // Lock to ensure thread safety

        if (head < tail) {  // Check if there are tasks to steal
            task = *tasks[head];  // Dereference the pointer to get the task
            head++;  // Increment the head to move to the next task
            pthread_mutex_unlock(&lock);  // Unlock after modification
            return true;
        }

        pthread_mutex_unlock(&lock);  // Unlock if no task is stolen
        return false;  // No tasks to steal
    }

    // Pop task from the worker's own deque (LIFO order)
    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(std::function<void()> &task) {
        pthread_mutex_lock(&lock);  // Lock to ensure thread safety

        if (tail > head) {  // Check if there are tasks in the deque
            tail--;  // Move the tail backward (LIFO)
            task = *tasks[tail];  // Dereference the pointer to get the task
            pthread_mutex_unlock(&lock);  // Unlock after modification
            return true;
        }

        pthread_mutex_unlock(&lock);  // Unlock if no task to pop
        return false;  // No tasks to pop
    }

    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::vector<pthread_t> workers;

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
        // master_thread = pthread_self(); // Main thread as master

        for (int i = 1; i < num_workers; ++i) {
            if (pthread_create(&workers[i], nullptr, (void*(*)(void*))worker_func, (void*)(intptr_t)i) != 0) {
                throw std::runtime_error("Failed to create worker thread");
            }
            // std::cout<<"Worker "<<i<<" created"<<std::endl;
        }
        // std::cout << "Quill runtime initialized with " << num_workers << " threads." << std::endl;
    }

    volatile int finish_counter = 0;
    void start_finish() {
        // Handle the start of the finish scope (no recursion allowed)
        finish_counter = 0;
        // cout<<"Finish Counter: "<<finish_counter<<endl;
    }

    thread_local int worker_id = 0; // main thread id and declaration too

    int get_worker_id() {
        return worker_id;
    }

    
    void async(std::function<void()> &&lambda) {
        // Lock finish counter (thread-safe operation)
        pthread_mutex_lock(&finish_counter_lock);
        finish_counter++;
        pthread_mutex_unlock(&finish_counter_lock);


        // Dynamically allocate task on the heap
        std::unique_ptr<std::function<void()>> task_ptr = std::make_unique<std::function<void()>>(std::move(lambda));

        // Push task pointer to the correct worker's deque
        worker_deques[get_worker_id()].push(std::move(*task_ptr));
    }


    void find_and_execute_task(int worker_id) {
        cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        std::function<void()> task;

        // Try to pop a task from the local deque
        if (worker_deques[worker_id].pop(task)) {
            task();
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);
            task = nullptr;  
        } 
        else {
            // Attempt to steal a task from other workers
            for (int i = 0; i < num_workers; ++i) {
                if (i != worker_id && worker_deques[i].steal(task)) {
                    task();
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
                    task = nullptr;  
                    return;
                }
            }
        }
    }

    void worker_func(void* arg) {
        worker_id = (intptr_t)arg;
        // std::cout << "Worker " << worker_id << " started" << std::endl;
        // cout<<"Shutdown"<<shutdown<<endl;
        while (!shutdown) {
            find_and_execute_task(worker_id);
            // Optionally, add a small sleep to reduce contention
            // usleep(100); 
        }
    }

    void end_finish() {
        // int main_thread_id = 0; // Assuming the main thread has ID 0
        // cout<<"I am main thread: "<<get_worker_id()<<endl;
        while (finish_counter != 0) {
            find_and_execute_task(get_worker_id());
        }
        // cout<<"I didnt got a chance"<<endl;
    }
    // Finalize the Quill runtime
    void finalize_runtime() {
        shutdown = true;
        cout<<"Shutting down"<<endl;
        for (int i = 1; i < num_workers; ++i) {
            pthread_join(workers[i], nullptr);  // Join all worker threads
        }
    }
    
}

















