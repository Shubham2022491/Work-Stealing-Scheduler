#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <chrono> 
#include <map>
#include <cstdint>
#include <pthread.h>


namespace quill {

    int num_workers = 1; 
    constexpr size_t DEQUE_SIZE = 500; 
    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;

    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::vector<pthread_t> workers;

    thread_local int worker_id = 0; 

    int get_worker_id() {
        return worker_id;
    }

    template <size_t DEQUE_SIZE>
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
        pthread_mutex_init(&lock, nullptr); 
    }

    template <size_t DEQUE_SIZE>
    // the push function will now take a task object
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
    // the pop function will now take a task object
    
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

    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::put_node_at_end_of_linkedlist(Linked_list_Node* node) {
        if (linked_list_head == nullptr) {
            // If the list is empty, the new node becomes the head
            linked_list_head = node;
        } else {
            // Traverse to the end of the list
            Linked_list_Node* current = linked_list_head;
            while (current->next != nullptr) {
                current = current->next;
            }
            current->next = node; // Append the node at the end
        }
    }

    void reset_worker_AC_counter(int tot_workers){
        for (int worker_id = 0; worker_id < tot_workers; ++worker_id){
            worker_deques[worker_id].AC = (worker_id)*(INT_MAX/tot_workers);
        }
    }

    void reset_worker_SC_counter(int tot_workers){
        for (int worker_id = 0; worker_id < tot_workers; ++worker_id){
            worker_deques[worker_id].SC = 0;
            worker_deques[worker_id].execution_index_of_array = 0;
        }
    }

    void list_aggregation(int tot_workers){
        // Initialize the head pointers for the aggregated lists
        Linked_list_Node* created_task_list[tot_workers] = {nullptr};
        Linked_list_Node* created_task_tail[tot_workers] = {nullptr}; // Track tail for efficient insertion

        for (int worker_id = 0; worker_id < tot_workers; ++worker_id) {
            Linked_list_Node* current = worker_deques[worker_id].linked_list_head;

            while (current != nullptr) {
                int creator_id = current->worker_who_created_this_task;

                // If no list exists for the creator, initialize it
                if (created_task_list[creator_id] == nullptr) {
                    created_task_list[creator_id] = current;
                    created_task_tail[creator_id] = current;
                } else {
                    // Append to the end of the creator's list
                    created_task_tail[creator_id]->next = current;
                    created_task_tail[creator_id] = current;
                }

                current = current->next; // Move to the next node
            }
        }

        // Null-terminate the lists to avoid cycles
        for (int i = 0; i < tot_workers; ++i) {
            if (created_task_tail[i] != nullptr) {
                created_task_tail[i]->next = nullptr;
            }
        }

        for (int i = 0; i < tot_workers; ++i) {
            worker_deques[i].linked_list_head = created_task_list[i];
        }

        // (Optional) Print the results for debugging
        // for (int i = 0; i < tot_workers; ++i) {
        //     std::cout << "Worker " << i << " created tasks: ";
        //     Linked_list_Node* node = created_task_list[i];
        //     while (node != nullptr) {
        //         std::cout << "Task " << node->task_id << " -> ";
        //         node = node->next;
        //     }
        //     std::cout << "NULL" << std::endl;
        // }    
    }

    // Function to split a linked list into two halves
    void splitList(Linked_list_Node* source, Linked_list_Node** front, Linked_list_Node** back) {
        if (source == nullptr || source->next == nullptr) {
            *front = source;
            *back = nullptr;
            return;
        }

        Linked_list_Node* slow = source;
        Linked_list_Node* fast = source->next;

        while (fast != nullptr) {
            fast = fast->next;
            if (fast != nullptr) {
                slow = slow->next;
                fast = fast->next;
            }
        }

        *front = source;
        *back = slow->next;
        slow->next = nullptr; // Split into two lists
    }

    // Function to merge two sorted linked lists
    Linked_list_Node* sortedMerge(Linked_list_Node* a, Linked_list_Node* b) {
        if (a == nullptr) return b;
        if (b == nullptr) return a;

        Linked_list_Node* result = nullptr;

        if (a->task_id <= b->task_id) {
            result = a;
            result->next = sortedMerge(a->next, b);
        } else {
            result = b;
            result->next = sortedMerge(a, b->next);
        }
        return result;
    }

    // Recursive merge sort for linked list
    Linked_list_Node* mergeSort(Linked_list_Node* head) {
        if (head == nullptr || head->next == nullptr) {
            return head;
        }

        Linked_list_Node* a;
        Linked_list_Node* b;

        // Split the list into two halves
        splitList(head, &a, &b);

        // Recursively sort the two halves
        a = mergeSort(a);
        b = mergeSort(b);

        // Merge the sorted halves
        return sortedMerge(a, b);
    }

    void list_sorting(int tot_workers){
        for (int i = 0; i < tot_workers; ++i) {
            worker_deques[i].linked_list_head = mergeSort(worker_deques[i].linked_list_head);
        }
    }

    void create_array_to_store_stolen_task(int tot_workers){
        for (int i = 0; i < tot_workers; ++i) {
            // If there are no stolen tasks, continue
            if (worker_deques[i].SC == 0) {
                worker_deques[i].stolen_tasks_array = nullptr;
                continue;
            }
            
            // Allocate memory dynamically based on SC (steal counter)
            worker_deques[i].stolen_tasks_array = new Task[worker_deques[i].SC];
            for (unsigned int j = 0; j < worker_deques[i].SC; ++j) {
                worker_deques[i].stolen_tasks_array[j].task = nullptr; 
            }
            
            // std::cout << "Created stolen task array of size " << worker_deques[i].SC 
            //           << " for Worker " << i << std::endl;
        }
    }

    static int tracing_enabled = false;
    static int replay_enabled = false;

    void start_tracing() {
        tracing_enabled = true;
        reset_worker_AC_counter(num_workers); // See Lecture #13, Slides #16
        /* Each worker’s AC value set to (workerID * UINT_MAX/numWorkers) */
        reset_worker_SC_counter(num_workers);
    }
    void print_linked_list(int worker_id, WorkerDeque<DEQUE_SIZE>* deque) {
        std::cout << "Worker " << worker_id << " Linked List:" << std::endl;
    
        Linked_list_Node* current = deque->linked_list_head;
        if (!current) {
            std::cout << "  (Empty)" << std::endl;
            return;
        }
    
        while (current != nullptr) {
            std::cout << "  Task ID: " << current->task_id
                      << ", Created By: " << current->worker_who_created_this_task
                      << ", Executed By: " << current->worker_who_executed_this_task
                      << ", steal_counter_worker_who_stole: " << current->steal_counter_worker_who_stole
                      << std::endl;
            current = current->next;
        }
    }

    void stop_tracing() {
        if(replay_enabled == false) {
            list_aggregation(num_workers); // See Lecture #13, Slides #35-36
            list_sorting(num_workers); // See Lecture #13, Slides #37
            create_array_to_store_stolen_task(num_workers); // See Lecture #13, Slides #39-40
            tracing_enabled = false;
            replay_enabled = true;
            reset_worker_AC_counter(num_workers);
            reset_worker_SC_counter(num_workers);

            // Print each worker's linked list
            for (int worker_id = 0; worker_id < num_workers; ++worker_id) {
                // print_linked_list(worker_id, &worker_deques[worker_id]);
                worker_deques[worker_id].head = 0;
                worker_deques[worker_id].tail = 0;
            }
        }

        
    }

    volatile bool shutdown = false;
    void init_runtime()  {
        const char* workers_env = std::getenv("QUILL_WORKERS");
        if (workers_env) {
            num_workers = std::stoi(workers_env);
        }
        if (num_workers < 1) {
            num_workers = 1; 
        }

        
        worker_deques.resize(num_workers);
        workers.resize(num_workers);
        worker_deques[0].SC_lock = PTHREAD_MUTEX_INITIALIZER;
        for (int i = 1; i <num_workers; ++i) {
            worker_deques[i].SC_lock = PTHREAD_MUTEX_INITIALIZER;
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
        reset_worker_AC_counter(num_workers);
        reset_worker_SC_counter(num_workers);
        for (int worker_id = 0; worker_id < num_workers; ++worker_id){
            worker_deques[worker_id].head = 0;
            worker_deques[worker_id].tail = 0;
        }
    }

    void async(std::function<void()> &&lambda) {
        // std::cout << "Async: " << std::endl;
        if (tracing_enabled){
            int to_push_id = get_worker_id();
            pthread_mutex_lock(&finish_counter_lock);
            finish_counter++;
            pthread_mutex_unlock(&finish_counter_lock);
            std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
            Task task;
            task.task = task_ptr;
            task.worker_who_created_this_task = to_push_id;
            
            worker_deques[to_push_id].AC+=1;
            task.ID = worker_deques[to_push_id].AC;
            worker_deques[to_push_id].push(task);
            // std::cout<< "workerid: "<<to_push_id<< " created task_id: "<<task.ID<<std::endl;
            return;
        }
        else if (replay_enabled){
            // std::cout << "replay_enabled: " <<replay_enabled<<std::endl;
            // put checks at each alternate line
            int to_push_id = get_worker_id();
            // std::cout<<"check1"<<std::endl;
            pthread_mutex_lock(&finish_counter_lock);
            finish_counter++;
            pthread_mutex_unlock(&finish_counter_lock);
            // std::cout<<"check2"<<std::endl;
            std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
            // std::cout<<"check3"<<std::endl;
            Task task;
            task.task = task_ptr;
            task.worker_who_created_this_task = to_push_id;
            worker_deques[to_push_id].AC+=1;
            task.ID = worker_deques[to_push_id].AC;
            
            // worker_deques[to_push_id].push(task);
            // get the id of the worker who stole this task
            // find that node in the linked list of this worker that correspons to task.ID
            // std::cout<<"check4"<<std::endl;
            Linked_list_Node* current_ = worker_deques[to_push_id].linked_list_head;
            // std::cout<<"check5"<<std::endl;
            // std::cout<< "Worker_id: " <<to_push_id<<" Id to find in linked list: "<<task.ID<<std::endl;
            // while (current_ != nullptr && current_->task_id != task.ID) {
            //     // std::cout<<current_->task_id<<std::endl;
            //     current_ = current_->next;
            // }
            while(current_ != nullptr && current_->task_id!=task.ID){
                current_ = current_->next;
            } 
            if (current_ == nullptr){
                worker_deques[to_push_id].push(task);
                // std::cout<< "workerid: "<<to_push_id<< " created task_id: "<<task.ID<<std::endl;
                return;
            }
            // std::cout<<"check6"<<std::endl;
            int id_worker_who_executed = current_->worker_who_executed_this_task;
            // std::cout<<"check7"<<std::endl;
            // NEED LOCK ON SC
            pthread_mutex_lock(&worker_deques[id_worker_who_executed].SC_lock);
            worker_deques[id_worker_who_executed].stolen_tasks_array[worker_deques[id_worker_who_executed].SC] = task;
            worker_deques[id_worker_who_executed].SC++;
            // std::cout<<"SC counter of the worker who stole: "<<worker_deques[id_worker_who_executed].SC<<std::endl;
            pthread_mutex_unlock(&worker_deques[id_worker_who_executed].SC_lock);
            // std::cout<<"Task_id: "<<task.ID<<" given task to: "<<id_worker_who_executed<<std::endl;
            
            // worker_deques[id_worker_who_executed].SC+=1;
            return;
        }
    }

    void find_and_execute_task(int worker_id) {
        // cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        Task task;

        if (worker_deques[worker_id].pop(task)) {
            // give a code that starts a timer here to check the execution time of the task
            (*task.task)();
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);  
            task.task = nullptr;
        } 
        else{
            if (tracing_enabled){
                for (int steal_worker_id = 0; steal_worker_id < num_workers; ++steal_worker_id) {
                    if (steal_worker_id != worker_id && worker_deques[steal_worker_id].steal(task)) {
                        // get executing worker id and make a node of struct Linked_list_Node and put that at the end of the linked list
                        // std::cout<<"~~~~~~~steal~~~~~~~"<<std::endl;
                        Linked_list_Node* node = new Linked_list_Node();
                        node->next = nullptr;
                        node->steal_counter_worker_who_stole = worker_deques[get_worker_id()].SC;
                        node->worker_who_created_this_task = task.worker_who_created_this_task;
                        node->task_id = task.ID;
                        node->worker_who_executed_this_task = get_worker_id();
                        worker_deques[get_worker_id()].SC += 1;
                        // now to put node at the end of linkedlist in the struct of worker
                        worker_deques[get_worker_id()].put_node_at_end_of_linkedlist(node);
                        (*task.task)();
                        pthread_mutex_lock(&finish_counter_lock);
                        --finish_counter;
                        pthread_mutex_unlock(&finish_counter_lock);
                        task.task = nullptr;  
                        return;
                    }
                }
            }
            else if (replay_enabled){
                // no stealing from other deques from the tail side, now give the same tasks to those who initially stole them.
                // pthread_mutex_lock(&worker_deques[worker_id].SC_lock);
                if (worker_deques[worker_id].stolen_tasks_array[worker_deques[worker_id].execution_index_of_array].task != nullptr){    //NOTE I SUSPECT THERE WILL BE A LOCK FOR SC
                    // worker_deques[worker_id].execution_index_of_array +=1;
                    
                    // execute task
                    int index = worker_deques[worker_id].execution_index_of_array; //this will be the index at which the task is.
                    // pthread_mutex_unlock(&worker_deques[worker_id].SC_lock);
                    (*(worker_deques[worker_id].stolen_tasks_array[index].task))(); 
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
                    worker_deques[worker_id].stolen_tasks_array[index].task = nullptr;  
                    worker_deques[worker_id].execution_index_of_array +=1;
                    return;
                }
                // pthread_mutex_unlock(&worker_deques[worker_id].SC_lock);
            }
        }
    }

    void worker_func(void* arg) {
        worker_id = (intptr_t)arg;
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
    void free_linked_list(Linked_list_Node* head) {
        while (head != nullptr) {
            Linked_list_Node* temp = head;
            head = head->next;
            delete temp;
        }
    }

    void finalize_runtime() {
        shutdown = true;
        // cout<<"Shutting down"<<endl;
        for (int i = 1; i < num_workers; ++i) {
            pthread_join(workers[i], nullptr);
        }

        // Free all linked lists for each worker
        for (int i = 0; i < num_workers; ++i) {
            Linked_list_Node* head = worker_deques[i].linked_list_head;
            free_linked_list(head);
            worker_deques[i].linked_list_head = nullptr; // Prevent dangling pointer
            // Free the stolen tasks array if allocated
            if (worker_deques[i].stolen_tasks_array != nullptr) {
                delete[] worker_deques[i].stolen_tasks_array;
                worker_deques[i].stolen_tasks_array = nullptr; // Prevent dangling pointer
            }
            pthread_mutex_destroy(&worker_deques[i].SC_lock);
        }
        pthread_mutex_destroy(&finish_counter_lock);
        std::cout << "All linked lists and array freed successfully." << std::endl;
    } 
}




