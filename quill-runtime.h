#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>
#include <array>
#include <climits>
#include <iostream>


namespace quill {

struct Task {
    std::function<void()> *task;
    int depth;
    unsigned int ID;
    int worker_who_created_this_task;
    double execution_time;
};

struct Linked_list_Node{
    int worker_who_created_this_task;
    int worker_who_executed_this_task;
    unsigned int task_id;
    unsigned int steal_counter_worker_who_stole;
    Linked_list_Node* next; // Pointer to the next node in the linked list
};


template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<Task, DEQUE_SIZE> tasks;
    // head pointer  
    Linked_list_Node* linked_list_head;
    volatile int head;   
    volatile int tail;   
    pthread_mutex_t lock; 
    int numa_domain;
    int numa_core_id;
    unsigned int AC;
    unsigned int SC;
    // Array to store stolen tasks
    Task* stolen_tasks_array;
    
    WorkerDeque();
    void push(Task task); 
    bool steal(Task &task); 
    bool pop(Task &task);
    void put_node_at_end_of_linkedlist(Linked_list_Node* node);
};



    extern int num_workers;                     
    extern std::vector<pthread_t> workers;    
    extern pthread_t master_thread;
    
    
    void worker_func(void* arg);
  

} // namespace quill

#endif 
