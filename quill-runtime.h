#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>

namespace quill {

struct Task {
    std::function<void()> *task;
    int depth;
    double execution_time;
};


template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<Task, DEQUE_SIZE> tasks;  
    volatile int head;   
    volatile int tail;   
    pthread_mutex_t lock; 
    int numa_domain;
    int numa_core_id;
    
    WorkerDeque();
    void push(Task task); 
    bool steal(Task &task); 
    bool pop(Task &task);
};



    extern int num_workers;                     
    extern std::vector<pthread_t> workers;    
    extern pthread_t master_thread;
    
    
    void worker_func(void* arg);
  

} // namespace quill

#endif 
