#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <chrono> 
#include <map>

#include <pthread.h>
using namespace std;


namespace quill {

    thread_local int task_depth = 0;
    int num_workers = 1; 
    constexpr size_t DEQUE_SIZE = 50;  
  

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;
    std::map<int, double> avg_time_per_level;  
    std::map<int, pthread_mutex_t> level_locks;  
    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::vector<pthread_t> workers;

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

        printf("Push ID: %d, Worker ID: %d\n", request_box, get_worker_id());

        if (nextTail == head) {  
            std::cerr << "Error: Worker deque is full! Cannot push more tasks." << std::endl;
            std::exit(EXIT_FAILURE); 
        }


        // Check if there's a pending request for a task
        if (request_box != -1) {

            int target_worker_id = request_box;
            pthread_mutex_lock(&lock);
            request_box = -1;  // Reset the request box
            pthread_mutex_unlock(&lock);

            //pthread_mutex_lock(&worker_deques[target_worker_id].lock);  // Lock the target deque's mutex

            // Assign the task to the requesting worker's mailbox
            worker_deques[target_worker_id].mail_box = tasks[head];
            head = (head + 1) % DEQUE_SIZE; 
            pthread_cond_signal(&worker_deques[target_worker_id].condition_wait);
            //pthread_mutex_unlock(&worker_deques[target_worker_id].lock);
            return; 
        }
        else{
            tasks[tail] = task;
            tail = nextTail;
        }

        // pthread_mutex_unlock(&lock);  // Unlock the current deque's mutex
    }

    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(Task &task, int i) {
        
        if (worker_deques[i].head == worker_deques[i].tail || worker_deques[i].request_box!=-1) {
            //pthread_mutex_unlock(&worker_deques[i].lock);  // Unlock the target deque's mutex
            return false;
        }

        pthread_mutex_lock(&worker_deques[i].lock); 
        int tim = get_worker_id();
        worker_deques[i].request_box = tim;
        pthread_mutex_unlock(&worker_deques[i].lock); 

        pthread_mutex_lock(&lock);  // Lock the target deque's mutex
        // Set the request box to notify the other worker
        // Wait until a task is pushed into the mailbox
        
        while (mail_box.task == nullptr) {  // Check if the mailbox is empty
            pthread_cond_wait(&condition_wait, &lock);
            //printf("Stuck in a loop\n");
        }
        printf("Steal ID: %d, Worker ID: %d, Request box status: %d, Head:%d, Tail: %d\n", i, get_worker_id(), worker_deques[i].request_box, worker_deques[i].head, worker_deques[i].tail);

        printf("Steal Successs\n");
        // Steal the task from the mailbox
        pthread_mutex_unlock(&lock);  // Unlock the target deque's mutex

        task = mail_box;
        mail_box = {nullptr,0,0};  // Reset the mailbox

        return true;
    }

    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(Task &task) {
        //pthread_mutex_lock(&lock);

        if (head == tail) { 
            //pthread_mutex_unlock(&lock);
            return false;
        }

        printf("Pop ID: %d, Worker ID: %d\n", request_box, get_worker_id());

        if (request_box!=-1){
    
            // Signal the requesting worker
            int target_worker_id = request_box;
            pthread_mutex_lock(&lock);
            request_box = -1;  // Reset the request box
            pthread_mutex_unlock(&lock);
            //pthread_mutex_lock(&worker_deques[target_worker_id].lock);
            worker_deques[target_worker_id].mail_box = tasks[head];
            head = (head + 1) % DEQUE_SIZE; 
            //request_box = -1;
            pthread_cond_signal(&worker_deques[target_worker_id].condition_wait);
            //pthread_mutex_unlock(&worker_deques[target_worker_id].lock);
            return true;
        }
        tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
        task = tasks[tail]; 
        return true;
        

        //pthread_mutex_unlock(&lock);
    }

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
                if (i != worker_id && worker_deques[worker_id].steal(task, i)) {
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
        std::cout << "Average time per level: ";
        for (const std::pair<int, double>p : avg_time_per_level) {
            std::cout << "{ " << p.first << " : " << p.second << " } ";
        }
        std::cout << std::endl;


    }
    
}

