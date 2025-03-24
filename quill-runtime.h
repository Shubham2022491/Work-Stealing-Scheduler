#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <pthread.h>
#include <functional>
#include <vector>
#include <memory>
#include <climits>
#include <array>

namespace quill {


struct Task{
    std::function<void()> *task;
    unsigned int id;
    int worker_who_created_task;
};

struct Linked_List_Node{
    unsigned int id;
    int worker_who_created_task;
    int worker_who_stole_task;
    unsigned int SC;
    Linked_List_Node* next_node;
};

template <size_t DEQUE_SIZE>
struct WorkerDeque {
    std::array<Task, DEQUE_SIZE> tasks;
    Linked_List_Node* list_head;
    volatile int head;   
    volatile int tail;    
    pthread_mutex_t lock; 
    unsigned int AC;
    unsigned int SC;
    std::vector<Task*> tasks_stolen_array;

    WorkerDeque();
    void push(Task task); 
    bool steal(Task &task); 
    bool pop(Task &task);
    void put_node_at_end_of_linkedlist(Linked_List_Node* node);

};

    extern int num_workers;                     
    extern std::vector<pthread_t> workers;    
    extern pthread_t master_thread;
    
    
    void worker_func(void* arg);
  

} // namespace quill

#endif 
