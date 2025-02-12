#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>

namespace quill {


template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<std::function<void()>*, DEQUE_SIZE> tasks;  
    volatile int head;   
    volatile int tail;   
    pthread_mutex_t lock; 

    WorkerDeque();
    void push(std::function<void()>* task); 
    bool steal(std::function<void()> &task); 
    bool pop(std::function<void()> &task);
};

    extern int num_workers;                     
    extern std::vector<pthread_t> workers;    
    extern pthread_t master_thread;
    
    
    void worker_func(void* arg);
  

} // namespace quill

#endif 
