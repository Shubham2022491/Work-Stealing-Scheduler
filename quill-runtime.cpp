#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <atomic>

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
        //list_head = nullptr;
    }

    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::put_node_at_end_of_linkedlist(Linked_List_Node* node){
        if(list_head == nullptr) list_head = node;
        else{
            Linked_List_Node* current = list_head;
            while(current->next_node != nullptr) current = current->next_node;
            current->next_node = node;
        }
        //cout<<list_head->worker_who_created_task<<" "<<list_head->worker_who_stole_task<<"CHECK\n";
    }

    
    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::push(Task task) {
        
        int nextTail = (tail + 1) % DEQUE_SIZE; 

        if (nextTail == head) {  
            std::cerr << "Error: Worker deque is full! Cannot push more tasks." << std::endl;
            std::exit(EXIT_FAILURE); 
        }

        tasks[tail] = task; 
        tail = nextTail; 
            
    }


    
    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(Task &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        task = tasks[head];
        head = (head + 1) % DEQUE_SIZE; 

        pthread_mutex_unlock(&lock);
        return true;
    }



    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(Task &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
        task = tasks[tail]; 

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

    void reset_AC_counter(){
        for(int worker_id=0; worker_id<num_workers; worker_id++){
            worker_deques[worker_id].AC = worker_id * INT_MAX/num_workers;
        }
    }

    void reset_SC_counter(){
        for(int worker_id=0; worker_id<num_workers; worker_id++){
            worker_deques[worker_id].SC = 0;
        }
    }


    std::atomic<bool> replay_enabled{false};
    std::atomic<bool> tracing_enabled{false};
    
    void start_tracing(){
        tracing_enabled = true;
        reset_AC_counter();
        reset_SC_counter();
    }

    void list_aggregation(){
        Linked_List_Node* current_linked_list[num_workers] = {nullptr};
        Linked_List_Node* current_linked_list_tail[num_workers] = {nullptr};
        
        for(int i=0; i<num_workers; i++){
            Linked_List_Node* curr_node = worker_deques[i].list_head;
            worker_deques[i].list_head = nullptr;
            while(curr_node != nullptr){
                //cout<<curr_node->worker_who_created_task<<" "<<i<<"\n";
                int task_creator = curr_node->worker_who_created_task;
                
                if(current_linked_list[task_creator] == nullptr){
                    current_linked_list[task_creator] = curr_node;
                    current_linked_list_tail[task_creator] = curr_node;
                    //current_linked_list_tail[task_creator] -> next_node = nullptr;
                }
                else{
                    current_linked_list_tail[task_creator]->next_node = curr_node;
                    current_linked_list_tail[task_creator]= curr_node;
                    //current_linked_list_tail[task_creator] -> next_node = nullptr;
                } 

                curr_node = curr_node -> next_node;
            }
        }


        for (int i = 0; i < num_workers; ++i) {
            if (current_linked_list_tail[i] != nullptr) {
                current_linked_list_tail[i]->next_node = nullptr;
            }
        }

        cout<<"KEEP A CHECKONE\n";

        for (int i = 0; i < num_workers; ++i) {
            worker_deques[i].aggregated_list_head = current_linked_list[i];
        }

        cout<<"KEEP A CHECKTWO\n";

    }

        // Function to split a linked list into two halves
    void splitList(Linked_List_Node* source, Linked_List_Node** front, Linked_List_Node** back) {
            if (source == nullptr || source->next_node == nullptr) {
                *front = source;
                *back = nullptr;
                return;
            }
    
            Linked_List_Node* slow = source;
            Linked_List_Node* fast = source->next_node;
    
            while (fast != nullptr) {
                                fast = fast->next_node;
                if (fast != nullptr) {
                    slow = slow->next_node;
                    fast = fast->next_node;
                }
            }
    
            *front = source;
            *back = slow->next_node;
            slow->next_node = nullptr; // Split into two lists
        }
    
        // Function to merge two sorted linked lists
        Linked_List_Node* sortedMerge(Linked_List_Node* a, Linked_List_Node* b) {
            if (a == nullptr) return b;
            if (b == nullptr) return a;
    
            Linked_List_Node* result = nullptr;
    
            if (a->id <= b->id) {
                result = a;
                result->next_node = sortedMerge(a->next_node, b);
            } else {
                result = b;
                result->next_node = sortedMerge(a, b->next_node);
            }
            return result;
        }
    
        // Recursive merge sort for linked list
        Linked_List_Node* mergeSort(Linked_List_Node* head) {
            if (head == nullptr || head->next_node == nullptr) {
                return head;
            }
    
            Linked_List_Node* a;
            Linked_List_Node* b;
    
            // Split the list into two halves
            splitList(head, &a, &b);
    
            // Recursively sort the two halves
            a = mergeSort(a);
            b = mergeSort(b);
    
            // Merge the sorted halves
            return sortedMerge(a, b);
        }
    
    
    void list_sorting(){
            for (int i = 0; i < num_workers; ++i) {
                worker_deques[i].aggregated_list_head = mergeSort(worker_deques[i].aggregated_list_head);
            }
        }
    
    void create_steal_array(){
        for(int i=0; i<num_workers; i++){
            if (worker_deques[i].SC == 0) 
            worker_deques[i].tasks_stolen_array = {nullptr};
            else{
                worker_deques[i].tasks_stolen_array.resize(worker_deques[i].SC, nullptr);
            }
            std::cout << "Created stolen task array of size " << worker_deques[i].SC 
                      << " for Worker " << i << std::endl;

        }
                    
    }

    void stop_tracing(){
        if (!replay_enabled.load(std::memory_order_relaxed)){
            cout<<"Tracing enabled\n";
            list_aggregation();
            cout<<"LIST AGGREGATED\n";
            list_sorting();
            cout<<"LIST SORTED\n";
            create_steal_array();
            cout<<"STEAL ARRAY CREATED\n";
            replay_enabled.store(true, std::memory_order_relaxed);
            tracing_enabled.store(false, std::memory_order_relaxed);
        }
    }

    thread_local int worker_id = 0; 

    int get_worker_id() {
        return worker_id;
    }

    
    void async(std::function<void()> &&lambda) {
     

        int worker_id = get_worker_id();
        if (tracing_enabled.load(std::memory_order_relaxed)){
            pthread_mutex_lock(&finish_counter_lock);
            finish_counter++;
            pthread_mutex_unlock(&finish_counter_lock);    
            std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
            Task task;
            task.task = task_ptr;
            task.id = worker_deques[worker_id].AC+1;
            task.worker_who_created_task = worker_id;
            pthread_mutex_lock(&worker_deques[worker_id].lock);
            worker_deques[worker_id].AC++;
            pthread_mutex_unlock(&worker_deques[worker_id].lock);
            worker_deques[worker_id].push(task);
            return;
        }
        else if(replay_enabled.load(std::memory_order_relaxed)){
            cout<<"REPLAY ENABLED CHECK "<<worker_id<<"\n";
            pthread_mutex_lock(&finish_counter_lock);
            finish_counter++;
            pthread_mutex_unlock(&finish_counter_lock);    
            std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
            Task task;
            task.task = task_ptr;
            task.id = worker_deques[worker_id].AC+1;
            task.worker_who_created_task = worker_id;
            worker_deques[worker_id].AC++;
            worker_deques[worker_id].push(task);


            Linked_List_Node* curr_node = worker_deques[worker_id].aggregated_list_head;
            while(curr_node!=nullptr && curr_node->id != task.id) curr_node = curr_node -> next_node;

            if(curr_node == nullptr){
                worker_deques[worker_id].push(task);
            }else{
                int id_worker_who_executed = curr_node->worker_who_stole_task;
                worker_deques[id_worker_who_executed].tasks_stolen_array[worker_deques[id_worker_who_executed].SC] = &task;
            }
        }
    }


    void find_and_execute_task(int worker_id) {
        // cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        Task task;


        if (worker_deques[worker_id].pop(task)) {
            (*task.task)();  
            // delete &task;  
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);  
            task.task = nullptr;
        } 
        else {
            if(tracing_enabled.load(std::memory_order_relaxed)){
                for (int i = 0; i < num_workers; ++i) {
                    if (i != worker_id && worker_deques[i].steal(task)) {
                        Linked_List_Node* node = new Linked_List_Node();
                        node->id = task.id;
                        node->worker_who_created_task = task.worker_who_created_task;
                        node->worker_who_stole_task = get_worker_id();
                        node->next_node = nullptr;
                        cout<<node->id<<"\n";
                        //pthread_mutex_lock(&worker_deques[worker_id].lock);
                        node->SC = worker_deques[get_worker_id()].SC;
                        worker_deques[get_worker_id()].SC++;
                       //pthread_mutex_unlock(&worker_deques[worker_id].lock);

                        worker_deques[get_worker_id()].put_node_at_end_of_linkedlist(node);
                        (*task.task)();
                        // delete &task;  
                        pthread_mutex_lock(&finish_counter_lock);
                        --finish_counter;
                        pthread_mutex_unlock(&finish_counter_lock);
                        task.task = nullptr;  
                        return;
                        }
                    }
            }else if (replay_enabled.load(std::memory_order_relaxed)) {
                cout<<"REPLAY STEAL CHECK "<<worker_id<<"\n";
                Task* task = worker_deques[worker_id].tasks_stolen_array[worker_deques[worker_id].SC]; // ✅ Get the task pointer
                
                if (task != nullptr) {  // Ensure the task is valid
                    //pthread_mutex_lock(&worker_deques[worker_id].lock); // Lock before modifying SC
                    int index = worker_deques[worker_id].SC;
                    worker_deques[worker_id].SC += 1;
                    //pthread_mutex_unlock(&worker_deques[worker_id].lock); // Unlock after modifying SC
            
                    (*(task->task))();
            
                    // Protect finish_counter
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
            
                    // Mark task as completed
                    worker_deques[worker_id].tasks_stolen_array[index] = nullptr;  
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

















