#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <chrono> 
#include <map>
#include <numa.h>
#include <cstdint>
#include <pthread.h>
//#define SIZE 4096 // Size of the array

namespace quill {

    thread_local int task_depth = 0;
    int num_workers = 1; 
    int num_numa_domains = 1;
    constexpr size_t DEQUE_SIZE = 500;  
    
  

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;
    // pthread_mutex_t temp_mut =  PTHREAD_MUTEX_INITIALIZER;
    std::map<int, double> avg_time_per_level;  
    std::map<int, pthread_mutex_t> level_locks;  
    // make a map where key will be integer(NUMA nodes) and the value will be a vector containing core_ids(int)
    std::map<int, std::vector<int>> numa_domains; // map of NUMA domains
    std::map<int,int> first_task_to_worker_id_NUMANODE_Workerid;

    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::vector<pthread_t> workers;
    std::vector<int*> numa_memory(num_numa_domains);
    std::vector<int> core_to_numa_mapping(num_workers);


    thread_local int worker_id = 0; 

    int get_worker_id() {
        return worker_id;
    }

    void init_mutex_for_level(int level) {
        static pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
        
        pthread_mutex_lock(&global_mutex);  // Lock global mutex to ensure safe initialization
        if (level_locks.find(level) == level_locks.end()) {
            pthread_mutex_t new_mutex = PTHREAD_MUTEX_INITIALIZER;
            level_locks[level] = new_mutex;  // Assign new mutex for the level
        }
        pthread_mutex_unlock(&global_mutex);
    }

    void update_avg_time(int level, double execution_time) {
        init_mutex_for_level(level);  // Ensure mutex exists for the level

        pthread_mutex_lock(&level_locks[level]);  // Lock mutex for this level

        if (avg_time_per_level.find(level) != avg_time_per_level.end()) {
            avg_time_per_level[level] = (avg_time_per_level[level] + execution_time) / 2.0;
        } else {
            avg_time_per_level[level] = execution_time;
        }

        pthread_mutex_unlock(&level_locks[level]);  // Unlock after updating
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
    // std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    // std::vector<pthread_t> workers;


    template <typename T>
    T* numa_alloc(size_t size, int node) {
        void* ptr = numa_alloc_onnode(size * sizeof(T), node);
        if (!ptr) {
            throw std::runtime_error("Failed to allocate memory on NUMA node");
        }
        return static_cast<T*>(ptr);
    }

    template <typename T>
    void numa_dealloc(T* pointer, size_t size) {
        if (pointer) {
            numa_free(pointer, size * sizeof(T));
        }
    }

    void setup_worker_deques() {
        int total_workers = num_numa_domains * num_workers;
        core_to_numa_mapping.resize(total_workers);
    
        for (int worker_id = 0; worker_id < total_workers; ++worker_id) {
            int core_id = worker_id; // Assuming 1-to-1 mapping for simplicity
            int numa_node = numa_node_of_cpu(core_id); // Get NUMA node of core
    
            // Store the NUMA node for this core
            core_to_numa_mapping[worker_id] = numa_node;
            numa_domains[numa_node].push_back(worker_id);
            // Allocate deque in the same NUMA domain as the core
            worker_deques[worker_id] = *numa_alloc<WorkerDeque<DEQUE_SIZE>>(1, numa_node);
    
            std::cout << "Worker " << worker_id << " assigned to core " << core_id
                      << " on NUMA node " << numa_node << std::endl;
        }
    }

    void allocate_numa_memory(size_t size) {
        // Get the number of available NUMA domains
        num_numa_domains = numa_max_node() + 1;
        if (num_numa_domains < 1) {
            num_numa_domains = 1; // Fallback to 1 NUMA domain if NUMA is not available
        }
    
        // Calculate the size of each portion
        printf("Size %zu\n", size);
        size_t portion_size = size / num_numa_domains;
    
        // Allocate memory on each NUMA domain using numa_alloc<T>
        for (int i = 0; i < num_numa_domains; i++) {
            numa_memory[i] = numa_alloc<int>(portion_size, i); // Use template function
            if (!numa_memory[i]) {
                std::cerr << "Failed to allocate memory on NUMA node " << i << std::endl;
                exit(1);
            }
        }
    }

    void deallocate_numa_memory(size_t size) {
        size_t portion_size = size / num_numa_domains;
    
        for (int i = 0; i < num_numa_domains; i++) {
            if (numa_memory[i]) {
                quill::numa_dealloc(numa_memory[i], portion_size);
                numa_memory[i] = nullptr; // Avoid dangling pointers
            }
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
            
            std::cout << "Created stolen task array of size " << worker_deques[i].SC 
                      << " for Worker " << i << std::endl;
        }
    }

    static int tracing_enabled = false;
    static int replay_enabled = false;


    void start_tracing() {
        tracing_enabled = true;
        reset_worker_AC_counter(num_numa_domains * num_workers); // See Lecture #13, Slides #16
        /* Each worker’s AC value set to (workerID * UINT_MAX/numWorkers) */
        reset_worker_SC_counter(num_numa_domains * num_workers);
    }


    void stop_tracing() {
        if(replay_enabled == false) {
            list_aggregation(num_numa_domains * num_workers); // See Lecture #13, Slides #35-36
            list_sorting(num_numa_domains * num_workers); // See Lecture #13, Slides #37
            create_array_to_store_stolen_task(num_numa_domains * num_workers); // See Lecture #13, Slides #39-40
            replay_enabled = true;
        }
    }

    

    volatile bool shutdown = false;
    void init_runtime(size_t size)  {
        const char* workers_env = std::getenv("QUILL_WORKERS");
        if (workers_env) {
            num_workers = std::stoi(workers_env);
        }
        if (num_workers < 1) {
            num_workers = 1; 
        }

        int total_workers = num_numa_domains*num_workers;
        worker_deques.resize(total_workers);
        workers.resize(total_workers);
        allocate_numa_memory(size);
        setup_worker_deques();
        for (int i = 1; i <total_workers; ++i) {
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

    
    void async(int top_most, int id, std::function<void()> &&lambda) {
        // if level closed then aggregate the task and finish its execution
        // else estimate the average time to complete the task at that level and then decide whether to execute the task or not
        // if there is no average time at that level then push the task into the deque of the worker
        // else check the average time of that level and then decide whether to execute the task or not
        // if the average time is less than the estimated time then push the task into the deque of the worker
        // else execute the task
        
        if (tracing_enabled){
            if (top_most == 0) {          // topmost = 0 means this task is broken from topmost by main thread to be pushed into a deque of any worker of a NUMA domain
                int to_push_id = id;
                if (avg_time_per_level.find(task_depth) != avg_time_per_level.end()) {
                    if (avg_time_per_level[task_depth] > 0.01) {
                        pthread_mutex_lock(&finish_counter_lock);
                        finish_counter++;
                        pthread_mutex_unlock(&finish_counter_lock);
                        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                        Task task;
                        task.task = task_ptr;
                        task.depth = task_depth;
                        task.execution_time = 0;
                        task.worker_who_created_this_task = to_push_id;
                        task.ID = worker_deques[to_push_id].AC+1;
                        worker_deques[to_push_id].AC+=1;
                        worker_deques[to_push_id].push(task);
                        return;
                    }
                    else{
                        lambda();
                    }
                }
                else {
                    pthread_mutex_lock(&finish_counter_lock);
                    finish_counter++;
                    pthread_mutex_unlock(&finish_counter_lock);
                    std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                    Task task;
                    task.task = task_ptr;
                    task.depth = task_depth;
                    task.execution_time = 0;
                    task.worker_who_created_this_task = to_push_id;
                    task.ID = worker_deques[to_push_id].AC+1;
                    worker_deques[to_push_id].AC+=1;
                    worker_deques[to_push_id].push(task);
                    return;
                }
            }
            else{
                int to_push_id = get_worker_id();
                if (avg_time_per_level.find(task_depth) != avg_time_per_level.end()) {
                    if (avg_time_per_level[task_depth] > 0.01) {
                        pthread_mutex_lock(&finish_counter_lock);
                        finish_counter++;
                        pthread_mutex_unlock(&finish_counter_lock);
                        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                        Task task;
                        task.task = task_ptr;
                        task.depth = task_depth;
                        task.execution_time = 0;
                        task.worker_who_created_this_task = to_push_id;
                        task.ID = worker_deques[to_push_id].AC+1;
                        worker_deques[to_push_id].AC+=1;
                        worker_deques[to_push_id].push(task);
                        return;
                    }
                    else{
                        lambda();
                    }
                }
                else {
                    pthread_mutex_lock(&finish_counter_lock);
                    finish_counter++;
                    pthread_mutex_unlock(&finish_counter_lock);
                    std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                    Task task;
                    task.task = task_ptr;
                    task.depth = task_depth;
                    task.execution_time = 0;
                    task.worker_who_created_this_task = to_push_id;
                    task.ID = worker_deques[to_push_id].AC+1;
                    worker_deques[to_push_id].AC+=1;
                    worker_deques[to_push_id].push(task);
                    return;
                }
            } 
        }
        else if (replay_enabled){
            if (top_most == 0) {          // topmost = 0 means this task is broken from topmost by main thread to be pushed into a deque of any worker of a NUMA domain
                int to_push_id = id;
                if (avg_time_per_level.find(task_depth) != avg_time_per_level.end()) {
                    if (avg_time_per_level[task_depth] > 0.01) {
                        pthread_mutex_lock(&finish_counter_lock);
                        finish_counter++;
                        pthread_mutex_unlock(&finish_counter_lock);
                        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                        Task task;
                        task.task = task_ptr;
                        task.depth = task_depth;
                        task.execution_time = 0;
                        task.worker_who_created_this_task = to_push_id;
                        task.ID = worker_deques[to_push_id].AC+1;
                        worker_deques[to_push_id].AC+=1;
                        // worker_deques[to_push_id].push(task);
                        // get the id of the worker who stole this task
                        // find that node in the linked list of this worker that correspons to task.ID
                        Linked_list_Node* current_ = worker_deques[to_push_id].linked_list_head;
                        while(current_->task_id!=task.ID){
                            current_ = current_->next;
                        } 
                        int id_worker_who_executed = current_->worker_who_executed_this_task;
                        worker_deques[id_worker_who_executed].stolen_tasks_array[worker_deques[id_worker_who_executed].SC] = task;
                        // worker_deques[id_worker_who_executed].SC+=1;
                        return;
                    }
                    else{
                        lambda();
                    }
                }
                else {
                    pthread_mutex_lock(&finish_counter_lock);
                    finish_counter++;
                    pthread_mutex_unlock(&finish_counter_lock);
                    std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                    Task task;
                    task.task = task_ptr;
                    task.depth = task_depth;
                    task.execution_time = 0;
                    task.worker_who_created_this_task = to_push_id;
                    task.ID = worker_deques[to_push_id].AC+1;
                    worker_deques[to_push_id].AC+=1;
                    // worker_deques[to_push_id].push(task);
                    // get the id of the worker who stole this task
                    // find that node in the linked list of this worker that correspons to task.ID
                    Linked_list_Node* current_ = worker_deques[to_push_id].linked_list_head;
                    while(current_->task_id!=task.ID){
                        current_ = current_->next;
                    } 
                    int id_worker_who_executed = current_->worker_who_executed_this_task;
                    worker_deques[id_worker_who_executed].stolen_tasks_array[worker_deques[id_worker_who_executed].SC] = task;
                    // worker_deques[id_worker_who_executed].SC+=1;
                    return;
                }
            }
            else{
                int to_push_id = get_worker_id();
                if (avg_time_per_level.find(task_depth) != avg_time_per_level.end()) {
                    if (avg_time_per_level[task_depth] > 0.01) {
                        pthread_mutex_lock(&finish_counter_lock);
                        finish_counter++;
                        pthread_mutex_unlock(&finish_counter_lock);
                        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                        Task task;
                        task.task = task_ptr;
                        task.depth = task_depth;
                        task.execution_time = 0;
                        task.worker_who_created_this_task = to_push_id;
                        task.ID = worker_deques[to_push_id].AC+1;
                        worker_deques[to_push_id].AC+=1;
                        // worker_deques[to_push_id].push(task);
                        // get the id of the worker who stole this task
                        // find that node in the linked list of this worker that correspons to task.ID
                        Linked_list_Node* current_ = worker_deques[to_push_id].linked_list_head;
                        while(current_->task_id!=task.ID){
                            current_ = current_->next;
                        } 
                        int id_worker_who_executed = current_->worker_who_executed_this_task;
                        worker_deques[id_worker_who_executed].stolen_tasks_array[worker_deques[id_worker_who_executed].SC] = task;
                        // worker_deques[id_worker_who_executed].SC+=1;
                        return;
                    }
                    else{
                        lambda();
                    }
                }
                else {
                    pthread_mutex_lock(&finish_counter_lock);
                    finish_counter++;
                    pthread_mutex_unlock(&finish_counter_lock);
                    std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));
                    Task task;
                    task.task = task_ptr;
                    task.depth = task_depth;
                    task.execution_time = 0;
                    task.worker_who_created_this_task = to_push_id;
                    task.ID = worker_deques[to_push_id].AC+1;
                    worker_deques[to_push_id].AC+=1;
                    // worker_deques[to_push_id].push(task);
                    // get the id of the worker who stole this task
                    // find that node in the linked list of this worker that correspons to task.ID
                    Linked_list_Node* current_ = worker_deques[to_push_id].linked_list_head;
                    while(current_->task_id!=task.ID){
                        current_ = current_->next;
                    } 
                    int id_worker_who_executed = current_->worker_who_executed_this_task;
                    worker_deques[id_worker_who_executed].stolen_tasks_array[worker_deques[id_worker_who_executed].SC] = task;
                    // worker_deques[id_worker_who_executed].SC+=1;
                    return;
                }
            } 
        }
    }

    // parallel_for implememtaion where it will take the top most task and break into number equal to NUMA domains, and then call async to put the task into deque of any worker
    // so it will call async with thread id itself (0) and worker id of the worker in which the task will be pushed
    // parallel_for function that splits work into chunks and processes asynchronously.
    void parallel_for(int lower, int upper, std::function<void(int, int)> &&body) {
        // Calculate the number of chunks needed
        // first get the number of NUMA DOMAINS
        int num_chunks = (upper - lower)/num_numa_domains;
        int i =0;
        if (tracing_enabled){
            for (const auto &pair : numa_domains){
                int chunk_start = lower + i * num_chunks;
                int chunk_end = std::min(upper, lower + (i + 1)*num_chunks);
                // randomly get any worker_id from pair.second vector
                int id = pair.second[rand() % pair.second.size()];          // worker_id to push the task to
                first_task_to_worker_id_NUMANODE_Workerid[pair.first] = id;
                async(0,id,[=]() {
                    // Execute the task (body) for the current chunk of work
                    body(chunk_start, chunk_end);
                });
                i+=1;
            }
        }
        else if (replay_enabled){
            for (const auto &pair : numa_domains){
                int chunk_start = lower + i * num_chunks;
                int chunk_end = std::min(upper, lower + (i + 1)*num_chunks);
                // randomly get any worker_id from pair.second vector
                // int id = pair.second[rand() % pair.second.size()];          // worker_id to push the task to
                // first_task_to_worker_id_NUMANODE_Workerid[pair.first] = id
                int id = first_task_to_worker_id_NUMANODE_Workerid[pair.first];
                async(0,id,[=]() {
                    // Execute the task (body) for the current chunk of work
                    body(chunk_start, chunk_end);
                });
                i+=1;
            }
        }
    }


    void find_and_execute_task(int worker_id) {
        // cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        Task task;

        if (worker_deques[worker_id].pop(task)) {
            // give a code that starts a timer here to check the execution time of the task
            task_depth = task.depth + 1;
            auto start_time = std::chrono::high_resolution_clock::now();
            (*task.task)();
            auto end_time = std::chrono::high_resolution_clock::now();
            task.execution_time = std::chrono::duration<double>(end_time - start_time).count();
            update_avg_time(task.depth, task.execution_time);
            
            // delete &task;  
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);  
            task.task = nullptr;
        } 
        else {
            // first check steal in its own numa domain
            // with help of worker ID, get its NUMA Domain, and then get all workerIDS RANGE IN that domain
            if (tracing_enabled){
                int Numa_node_of_worker = core_to_numa_mapping[worker_id];
                // Now using this Numa_node_of_worker select randomly any id 
                int steal_worker_id = -1;
                while(steal_worker_id==-1 || steal_worker_id == worker_id){
                    steal_worker_id = numa_domains[Numa_node_of_worker][rand() % numa_domains[Numa_node_of_worker].size()];
                }
                if (worker_deques[steal_worker_id].steal(task)) {
                    // get executing worker id and make a node of struct Linked_list_Node and put that at the end of the linked list
                    Linked_list_Node* node = new Linked_list_Node();
                    node->next = nullptr;
                    node->steal_counter_worker_who_stole = worker_deques[get_worker_id()].SC;
                    node->worker_who_created_this_task = task.worker_who_created_this_task;
                    node->task_id = task.ID;
                    node->worker_who_executed_this_task = get_worker_id();
                    worker_deques[get_worker_id()].SC += 1;
                    // now to put node at the end of linkedlist in the struct of worker
                    worker_deques[get_worker_id()].put_node_at_end_of_linkedlist(node);
                    task_depth = task.depth + 1;
                    auto start_time = std::chrono::high_resolution_clock::now();
                    (*task.task)();
                    auto end_time = std::chrono::high_resolution_clock::now();
                    task.execution_time = std::chrono::duration<double>(end_time - start_time).count();
                    update_avg_time(task.depth, task.execution_time);
                    // delete &task;  
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
                    task.task = nullptr;  
                    return;
                }
                else{
                    // if not steal from local NUMA , then check from other NUMA DOMAINS
                    for (const auto &pair : numa_domains){
                        if (pair.second.size() > 0 && pair.first != Numa_node_of_worker){
                            steal_worker_id = pair.second[rand() % pair.second.size()];
                            break;
                        }
                        else{
                            return;
                        }
                    }
                    if (worker_deques[steal_worker_id].steal(task)) {
                        Linked_list_Node* node = new Linked_list_Node();
                        node->next = nullptr;
                        node->steal_counter_worker_who_stole = worker_deques[get_worker_id()].SC;
                        node->worker_who_created_this_task = task.worker_who_created_this_task;
                        node->task_id = task.ID;
                        node->worker_who_executed_this_task = get_worker_id();
                        worker_deques[get_worker_id()].SC += 1;
                        // now to put node at the end of linkedlist in the struct of worker
                        worker_deques[get_worker_id()].put_node_at_end_of_linkedlist(node);
                        task_depth = task.depth + 1;
                        auto start_time = std::chrono::high_resolution_clock::now();
                        (*task.task)();
                        auto end_time = std::chrono::high_resolution_clock::now();
                        task.execution_time = std::chrono::duration<double>(end_time - start_time).count();
                        update_avg_time(task.depth, task.execution_time);
                        // delete &task;  
                        pthread_mutex_lock(&finish_counter_lock);
                        --finish_counter;
                        pthread_mutex_unlock(&finish_counter_lock);
                        task.task = nullptr;  
                        return;
                    }
                }
            }
            else if(replay_enabled){
                // no stealing from other deques from the tail side, now give the same tasks to those who initially stole them.
                if (worker_deques[worker_id].stolen_tasks_array[worker_deques[worker_id].SC].task != nullptr){    //NOTE I SUSPECT THERE WILL BE A LOCK FOR SC
                    worker_deques[worker_id].SC +=1;
                    // execute task
                    int index = worker_deques[worker_id].SC - 1; //this will be the index at which the task is.
                    task_depth = worker_deques[worker_id].stolen_tasks_array[index].depth + 1;
                    auto start_time = std::chrono::high_resolution_clock::now();
                    (*(worker_deques[worker_id].stolen_tasks_array[index].task))(); 
                    auto end_time = std::chrono::high_resolution_clock::now();
                    worker_deques[worker_id].stolen_tasks_array[index].execution_time = std::chrono::duration<double>(end_time - start_time).count();
                    update_avg_time(worker_deques[worker_id].stolen_tasks_array[index].depth, worker_deques[worker_id].stolen_tasks_array[index].execution_time);
         
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
                    worker_deques[worker_id].stolen_tasks_array[index].task = nullptr;  
                }
            }
        }
    }

    void worker_func(void* arg) {
        worker_id = (intptr_t)arg;
        int numa_domain = worker_id / num_workers;
        int core_id = worker_id % num_workers;

        // Bind thread to NUMA domain
        numa_run_on_node(numa_domain);

        // Bind thread to a specific core within the NUMA domain
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);

        sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
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
    
    void finalize_runtime(size_t size) {
        shutdown = true;
        // cout<<"Shutting down"<<endl;
        for (int i = 1; i < num_workers; ++i) {
            pthread_join(workers[i], nullptr);
        }
        std::cout << "Average time per level: ";
        for (const std::pair<int, double>p : avg_time_per_level) {
            std::cout << "{ " << p.first << " : " << p.second << " } ";
        }
        std::cout << std::endl;
        deallocate_numa_memory(size);
    } 
}

















