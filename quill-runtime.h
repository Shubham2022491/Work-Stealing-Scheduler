#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>
#include <array>

namespace quill {

    struct Task {
        std::function<void()> *task;
        int depth;
        double execution_time;
    
        // Constructor for Task
        Task(std::function<void()>* t = nullptr, int d = 0, double exec_time = 0.0)
            : task(t), depth(d), execution_time(exec_time) {}
    };
    


template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<Task, DEQUE_SIZE> tasks;  
    volatile int head;   
    volatile int tail;   
    pthread_mutex_t lock; 
    
    volatile bool flag = false; 
    
    pthread_cond_t condition_wait;
    volatile int request_box;
    Task mail_box;

    WorkerDeque();
    void push(Task task); 
    bool steal(Task &task, int i); 
    bool pop(Task &task);
    
};



    extern int num_workers;                     
    extern std::vector<pthread_t> workers;    
    extern pthread_t master_thread;
    
    
    void worker_func(void* arg);
  

} // namespace quill

#endif 