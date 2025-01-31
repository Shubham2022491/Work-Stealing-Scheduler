

#include "quill.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>
#include <functional>
#include <vector>
#include <memory>
#include <future>
#include <mutex>
#include <condition_variable>

using namespace std;

const int DEQUE_SIZE = 100;
struct WorkerDeque {
    std::array<std::unique_ptr<std::function<void()>>, DEQUE_SIZE> tasks;  // Array of task pointers
    int head;   // Index for the top (head) - accessed by thieves in FIFO order
    int tail;   // Index for the bottom (tail) - private to the worker, LIFO order
    pthread_mutex_t lock; // Mutex for thread-safe operations

    WorkerDeque();
    void push(std::function<void()> task);  // Push task for the worker (LIFO)
    bool steal(std::function<void()> &task);  // Steal task from another worker (FIFO)
    bool pop(std::function<void()> &task);  // Pop task from worker's own deque (LIFO)
};

volatile bool shutdown = false;
volatile int finish_counter = 0;
static int num_workers = 1;
vector<pthread_t> threads;
vector<WorkerDeque> worker_deques;
pthread_t master_thread;  
pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;





// typedef struct{
//     uint64_t input;
// }thread_args;

// typedef struct{
    
// }Task;


// void example_task(int n){
//     printf("Thread executed value: %d\n", n);
//     sleep(1);
//     return;
// }

// void* thread_func(void* ptr){
//     uint64_t i = ((thread_args*)ptr)-> input;
//     example_task(i);
//     return NULL;
// }

// void close_threads(uint64_t num_threads, pthread_t* threads){
//     int status;
//     for(int i=1; i<num_threads; i++){
//         status = pthread_join(threads[i], NULL);
//         if (status!=0) fprintf(stderr, "Issue with joining thread\n");
//     }    
// }

// void init_threads(){
//     const int num_threads = 8;
//     pthread_t threads[num_threads];
//     thread_args args[num_threads];
//     int status;
//     for(int i=1; i<num_threads; i++){
//         args[i].input= i;
//         pthread_create(&threads[i], NULL, thread_func, (void*)&args[i]);
//         if (status!=0) fprintf(stderr, "Issue with creating thread\n");
//     }
//     close_threads(num_threads, threads);
// }

// void start_finish(){
//     finish_counter = 0;
// }

// void async(Task* task){
//     finish_counter++;
// }

WorkerDeque::WorkerDeque() {
    head = 0; tail = 0;
    pthread_mutex_init(&lock, nullptr);  // Initialize the mutex
}

bool WorkerDeque::pop(std::function<void()> &task) {
    
    if (tail == head) return false;
    pthread_mutex_lock(&lock);
    tail--; 
    task = std::move(*tasks[tail]); 
    tasks[tail].reset();  
    pthread_mutex_unlock(&lock);
    return true;
}


bool WorkerDeque::steal(std::function<void()> &task) {
    
    if (tail == head) return false;
    
}


void find_and_execute_task(int thread) {
    function<void()> task;

    if (worker_deques[thread].pop(task)) {
        task();  
        task = nullptr;  
        pthread_mutex_lock(&finish_counter_lock);
        finish_counter--;
        pthread_mutex_unlock(&finish_counter_lock);
    }else{
        int random_index = rand()%num_workers;
        if(random_index!=thread && worker_deques[thread].steal(task)){
            task();
            task = nullptr;
        }
    }
}

void worker_routine(int thread){
    while(!shutdown){
        find_and_execute_task(thread);
    }
}

void* thread_func(void *args){
    int i = *((int*)args);
    worker_routine(i);
    return NULL; 
}

void init_runtime(){
        const char* workers_env = std::getenv("QUILL_WORKERS");
        int status;
        if (workers_env) {
            num_workers = std::stoi(workers_env);
        }
        if (num_workers < 1) {
            num_workers = 1;
        }

        worker_deques.resize(num_workers);
        threads.resize(num_workers);

        for(int i=0; i<num_workers; i++){
            status = pthread_create(&threads[i], NULL, thread_func, (void *)&i);
            if(status!=0)throw std::runtime_error("Failed to create worker thread");
        }
}


int fibonacci(int n) {
    if (n < 2) return n;

    int* x;
    int a;
    quill::async([&]() {
        *x = fibonacci(n - 1);
        a = *x;
    });

    int y = fibonacci(n - 2);

    return *x + y;
}

int main(int argc, char* argv[]) {
    quill::init_runtime();
    int n = 8;
    int result;

    if(argc > 1) n = atoi(argv[1]);

    quill::start_finish();
    result = fibonacci(n);
    quill::end_finish();

    printf("Result of the fibonacci: %d\n", result);
    quill::finalize_runtime();
    return 0;
}
