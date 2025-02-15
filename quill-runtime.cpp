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

// What to do next for depth
// first wrap task into a struct----> pointer to the function to execute on heap, depth, execution time.
// struct Task {
//     std::function<void()> *task;
//     int depth;
//     double execution_time;
// };

// make a global hasp map for each level average time it takes to complete the task at that level
// and implement the ATC Algo

namespace quill {

    thread_local int task_depth = 0;
    int num_workers = 1; 
    constexpr size_t DEQUE_SIZE = 50;  
  

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t task_depth_lock = PTHREAD_MUTEX_INITIALIZER;

    // make a global hasp map for each level average time it takes to complete the task at that level
    // and implement the ATC Algo
    std::map<int, double> avg_time_per_level;
    // make a function that takes index of the level and the execution time of the task and updates the average time of that level
    void update_avg_time(int level, double execution_time) {
        if (avg_time_per_level.find(level) != avg_time_per_level.end()) {
            // apply locks
            pthread_mutex_lock(&task_depth_lock);
            avg_time_per_level[level] = (avg_time_per_level[level] + execution_time) / 2;
            pthread_mutex_unlock(&task_depth_lock);
        }
        else {
            pthread_mutex_lock(&task_depth_lock);
            avg_time_per_level[level] = execution_time;
            pthread_mutex_unlock(&task_depth_lock);
        }
    }

    template <size_t DEQUE_SIZE>
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
        pthread_mutex_init(&lock, nullptr); 
    }

    
    template <size_t DEQUE_SIZE>
    // the push function will now take a task object, change the signature gpt

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
    // the pop function will now take a task object, change the signature gpt
    
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

    thread_local int worker_id = 0; 

    int get_worker_id() {
        return worker_id;
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
           
            for (int i = 0; i < num_workers; ++i) {
                if (i != worker_id && worker_deques[i].steal(task)) {
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

















