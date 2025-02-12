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

    int num_workers = 1; 
    constexpr size_t DEQUE_SIZE = 50;  
  

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;


    template <size_t DEQUE_SIZE>
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
        pthread_mutex_init(&lock, nullptr); 
    }

    
    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::push(std::function<void()>*task) {
        // pthread_mutex_lock(&lock);  

        // if (tail < DEQUE_SIZE) {
        //     tasks[tail] = task ;  
        //     tail++;
        // }
        // else {
        //     // cout<<"WorkerDeque overflow: Cannot push, deque is full!"<<endl;
        //     throw std::runtime_error("WorkerDeque overflow: Cannot push, deque is full!");
        // }
        int nextTail = (tail + 1) % DEQUE_SIZE; 

        if (nextTail == head) {  
            std::cerr << "Error: Worker deque is full! Cannot push more tasks." << std::endl;
            std::exit(EXIT_FAILURE); 
        }

        tasks[tail] = task; 
        tail = nextTail; 
            
    }

    
    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(std::function<void()> &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        task = *tasks[head];
        head = (head + 1) % DEQUE_SIZE; 

        pthread_mutex_unlock(&lock);
        return true;
    }


    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(std::function<void()> &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
        task = *tasks[tail]; 

        pthread_mutex_unlock(&lock);
        return true;
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
            num_workers = 1; 
        }
        worker_deques.resize(num_workers);
        workers.resize(num_workers);
        // master_thread = pthread_self(); 

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
        finish_counter = 0;
        // cout<<"Finish Counter: "<<finish_counter<<endl;
    }

    thread_local int worker_id = 0; 

    int get_worker_id() {
        return worker_id;
    }

    
    void async(std::function<void()> &&lambda) {
     
        pthread_mutex_lock(&finish_counter_lock);
        finish_counter++;
        pthread_mutex_unlock(&finish_counter_lock);


        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));

 
        worker_deques[get_worker_id()].push(task_ptr);
    }


    void find_and_execute_task(int worker_id) {
        // cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        std::function<void()> task;


        if (worker_deques[worker_id].pop(task)) {
            task();  
            // delete &task;  
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);  
            task = nullptr;
        } 
        else {
           
            for (int i = 0; i < num_workers; ++i) {
                if (i != worker_id && worker_deques[i].steal(task)) {
                    task();
                    // delete &task;  
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
        }
    }

    void end_finish() {
        // int main_thread_id = 0; 
        // cout<<"I am main thread: "<<get_worker_id()<<endl;
        while (finish_counter != 0) {
            find_and_execute_task(get_worker_id());
        }
        // cout<<"I didnt got a chance"<<endl;
    }
    
    void finalize_runtime() {
        shutdown = true;
        // cout<<"Shutting down"<<endl;
        for (int i = 1; i < num_workers; ++i) {
            pthread_join(workers[i], nullptr);
        }
    }
    
}

















