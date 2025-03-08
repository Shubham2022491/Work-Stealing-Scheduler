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

#include <pthread.h>
using namespace std;
#define SIZE 10485760 // Size of the array

namespace quill {

    thread_local int task_depth = 0;
    int num_workers = 1; 
    int num_numa_domains = 1;
    constexpr size_t DEQUE_SIZE = 50;  
  

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;
    // pthread_mutex_t temp_mut =  PTHREAD_MUTEX_INITIALIZER;
    std::map<int, double> avg_time_per_level;  
    std::map<int, pthread_mutex_t> level_locks;  
    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::vector<pthread_t> workers;
    std::vector<int*> numa_memory(num_numa_domains);
    std::vector<int> core_to_numa_mapping(NUM_WORKERS);


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
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0), request_box(-1), mail_box(nullptr, 0, 0) {
        pthread_mutex_init(&lock, nullptr); 
        pthread_cond_init(&condition_wait, nullptr);
    }

    
    // the push function will now take a task object
    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::push(Task task) {

        int nextTail = (tail + 1) % DEQUE_SIZE;

        // printf("Push ID: %d, Worker ID: %d, Head:%d, Tail:%d\n", request_box, get_worker_id(), head, tail);

        if (nextTail == head) {  
            std::cerr << "Error: Worker deque is full! Cannot push more tasks." << std::endl;
            std::exit(EXIT_FAILURE); 
        }


        // Check if there's a pending request for a task

        // printf("Request box: %d\n", request_box);
        if (request_box != -1) {

            int target_worker_id = request_box;
            // if deque empty --> give the current task to thief
            // else give task from head to thief and push the current task to tail
            //pthread_mutex_lock(&worker_deques[target_worker_id].lock);  // Lock the target deque's mutex

            // Assign the task to the requesting worker's mailbox


            //pthread_mutex_lock(&lock); 
            if(head==tail) {
                worker_deques[target_worker_id].mail_box = task;
            }
            else{
                worker_deques[target_worker_id].mail_box = tasks[head];
            }
            pthread_cond_signal(&worker_deques[target_worker_id].condition_wait);
            //pthread_mutex_unlock(&lock);

            request_box = -1;  // Reset the request box

            

            if(head!=tail){
            head = (head + 1) % DEQUE_SIZE;
            tasks[tail] = task;
            tail = nextTail;
            }

            // pthread_mutex_lock(&lock);
            // request_box = -1;  // Reset the request box
            // pthread_mutex_unlock(&lock);
            //return; 
        }
        else{
        tasks[tail] = task;
        tail = nextTail;
        }
        // pthread_mutex_unlock(&lock);  // Unlock the current deque's mutex
    }

    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(Task &task, int worker_id) {
        
        if (head == tail) {
            //pthread_mutex_unlock(&worker_deques[i].lock);  // Unlock the target deque's mutex
            return false;
        }

        // pthread_mutex_lock(&lock); 
        // // int tim = get_worker_id();
        // request_box = worker_id;
        // pthread_mutex_unlock(&lock);
        // pthread_mutex_unlock(&lock); 
        // printf("Steal Request box:%d\n", request_box);

        pthread_mutex_lock(&worker_deques[worker_id].lock);  // Lock the target deque's mutex

        if(request_box!=-1){
            pthread_mutex_unlock(&worker_deques[worker_id].lock);
            return false;  // Nothing was sent
        }
        request_box = worker_id;
        // Set the request box to notify the other worker
        // Wait until a task is pushed into the mailbox
        // printf("HMMMMM");
        pthread_cond_wait(&worker_deques[worker_id].condition_wait, &worker_deques[worker_id].lock);  // Check if the mailbox is empty
        if (worker_deques[worker_id].mail_box.task == nullptr) {
            pthread_mutex_unlock(&worker_deques[worker_id].lock);
            return false;  // Nothing was sent
        }
    
            // if (worker_deques[worker_id].flag){
            //     printf("HOW\n");
            //     return false;
            // }
            // printf("I am before cond_wait");
            // pthread_cond_wait(&worker_deques[worker_id].condition_wait, &worker_deques[worker_id].lock);
            // printf("Cond_Wait value:%d Thread:%d", val, worker_id);
        // printf("Stuck in a loop\n");
        // printf("Steal ID: %d, Worker ID: %d, Request box status: %d, Head:%d, Tail: %d\n", worker_id, get_worker_id(), request_box, head, tail);

        //printf("Steal Successs\n");
        // Steal the task from the mailbox
          // Unlock the target deque's mutex
        // printf("Returning true");
        pthread_mutex_unlock(&worker_deques[worker_id].lock);  // Unlock
        task = worker_deques[worker_id].mail_box;
        worker_deques[worker_id].mail_box = {nullptr,0,0.0};  // Reset the mailbox
        return true;
    }

    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(Task &task) {
        //pthread_mutex_lock(&lock);

        if (head == tail) { 
            //pthread_mutex_unlock(&lock);
            return false;
        }

        // printf("Pop ID: %d, Worker ID: %d, Head:%d, Tail:%d\n", request_box, get_worker_id(), head, tail);

        if (request_box!=-1){
    
            // Signal the requesting worker
            //request_box = -1;
            int target_worker_id = request_box;

            //pthread_mutex_lock(&worker_deques[target_worker_id].lock);
            //pthread_mutex_lock(&lock);
            worker_deques[target_worker_id].mail_box = tasks[head];
            pthread_cond_signal(&worker_deques[target_worker_id].condition_wait);
            //pthread_mutex_unlock(&lock);

            request_box = -1;

            head = (head + 1) % DEQUE_SIZE; 

            // pthread_mutex_lock(&lock);
            
            // pthread_mutex_unlock(&lock);

            if(head==tail) return false;
            else{
                tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
                task = tasks[tail]; 
                return true;    
            }
            
        }else{
            tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
            task = tasks[tail]; 
            return true;
        }
        //pthread_mutex_unlock(&lock);
    }


    template <typename T>
    T* numa_alloc(size_t size, int node) {
        void* ptr = numa_alloc_onnode(size * sizeof(T), node);
        if (!ptr) {
            throw std::runtime_error("Failed to allocate memory on NUMA node");
        }
        return static_cast<T*>(ptr);
    }

    void setup_worker_deques() {
        int total_workers = num_numa_domains * num_workers;
        core_to_numa_mapping.resize(total_workers);
    
        for (int worker_id = 0; worker_id < total_workers; ++worker_id) {
            int core_id = worker_id; // Assuming 1-to-1 mapping for simplicity
            int numa_node = numa_node_of_cpu(core_id); // Get NUMA node of core
    
            // Store the NUMA node for this core
            core_to_numa_mapping[worker_id] = numa_node;
    
            // Allocate deque in the same NUMA domain as the core
            worker_deques[worker_id] = numa_alloc<WorkerDeque>(1, numa_node);
    
            std::cout << "Worker " << worker_id << " assigned to core " << core_id
                      << " on NUMA node " << numa_node << std::endl;
        }
    }

    void allocate_numa_memory() {
        // Get the number of available NUMA domains
        num_numa_domains = numa_max_node() + 1;
        if (num_numa_domains < 1) {
            num_numa_domains = 1; // Fallback to 1 NUMA domain if NUMA is not available
        }
    
        // Calculate the size of each portion
        size_t portion_size = SIZE / num_numa_domains;
    
        // Allocate memory on each NUMA domain
        for (int i = 0; i < num_numa_domains; i++) {
            numa_memory[i] = (int*)numa_alloc_onnode(portion_size, i);
            if (!numa_memory[i]) {
                std::cerr << "Failed to allocate memory on NUMA node " << i << std::endl;
                exit(1);
            }
        }
    }

    volatile bool shutdown = false;
    void init_runtime() {
        const char* domain_env = std::getenv("NUMA_DOMAINS");
        const char* workers_env = std::getenv("QUILL_WORKERS");
        if(domain_env){
            num_numa_domains = std::stoi(workers_env);
        }
        if(num_numa_domains < 1){
            num_numa_domains = 1;
        }
        if (workers_env) {
            num_workers = std::stoi(workers_env);
        }
        if (num_workers < 1) {
            num_workers = 1; 
        }

        int total_workers = num_numa_domains*num_workers;
        worker_deques.resize(total_workers);
        workers.resize(total_workers);
        allocate_numa_memory();
        
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

    
    void async(std::function<void()> &&lambda) {
        // if level closed then aggregate the task and finish its execution
        // else estimate the average time to complete the task at that level and then decide whether to execute the task or not
        // if there is no average time at that level then push the task into the deque of the worker
        // else check the average time of that level and then decide whether to execute the task or not
        // if the average time is less than the estimated time then push the task into the deque of the worker
        // else execute the task
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
                worker_deques[get_worker_id()].push(task);
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
            worker_deques[get_worker_id()].push(task);
            return;
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
                int i = rand() % num_workers;
                if (i != worker_id && worker_deques[i].steal(task, worker_id)) {
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

    void worker_func(void* arg) {
        worker_id = (intptr_t)arg;
        int numa_domain = worker_id / num_numa_domains;
        int core_id = worker_id % num_numa_domains;

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
    
    void finalize_runtime() {
        shutdown = true;
        cout<<"Shutting down"<<endl;
        for (int i = 0; i < num_workers; ++i) {
            // printf("Finish runtime: %d\n", i);
            worker_deques[i].flag = true;

            // pthread_mutex_lock(&worker_deques[i].lock); 
            // printf("Cond_Val %d\n", worker_deques[i].condition_wait);
            pthread_cond_signal(&worker_deques[i].condition_wait);
            // pthread_mutex_unlock(&worker_deques[i].lock); 
            // pthread_join(workers[i], nullptr);
        }
        for (int i = 1; i < num_workers; ++i) {
            // pthread_cond_signal(&worker_deques[i].condition_wait);
            // printf("l%d\n",i);
            pthread_join(workers[i], nullptr);
        }
        std::cout << "Average time per level: ";
        for (const std::pair<int, double>p : avg_time_per_level) {
            std::cout << "{ " << p.first << " : " << p.second << " } ";
        }
        std::cout << std::endl;


    }
    
}
