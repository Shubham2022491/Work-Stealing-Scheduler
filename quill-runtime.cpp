#include "quill-runtime.h"
#include "quill.h" // Include the core async operations from quill.h
#include <unistd.h>  // Required for usl
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <array>
#include <queue>  // Add queue header
#include <algorithm>
#include <random>

#include <pthread.h>
using namespace std;

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

#include <omp.h> //REMOVE IF YOU ARE NOT USING THE PROVIDED FIB EXAMPLE

#define MSR_RAPL_POWER_UNIT_AMD   	0xC0010299
#define MSR_PKG_ENERGY_STATUS_AMD 	0xC001029B  
#define MSR_RAPL_POWER_UNIT_INTEL   	0x606
#define MSR_PKG_ENERGY_STATUS_INTEL 	0x611
#define INTEL	0
#define AMD	1
#define INSTR_EVENT "PERF_COUNT_HW_INSTRUCTIONS"
int PROCESSOR	= -1;
int* instr_fd;
uint64_t* instr_prev, *energy_prev;
int NUM_PHYSICAL_CORES_PER_SOCKET=0, NUM_THREADS_PER_PHYSICAL_CORE=0, NUM_SOCKETS=0, NUM_CORES=0, NUM_LOGICAL_CORES=0;
int* socket_core = NULL; // first core id at each socket
double rapl_joule_unit = 0;
uint64_t* energy_start;
double start_main_timer;
double end_main_timer;

void get_timer(double *timer){
  struct timespec currentTime;
  clock_gettime (CLOCK_MONOTONIC, &currentTime);
  *timer = (currentTime.tv_sec + (currentTime.tv_nsec * 10e-10));
}

// Function to read an MSR register
uint64_t read_msr(int cpu, uint32_t msr) {
    char msr_path[32];
    snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%d/msr", cpu);
    int fd = open(msr_path, O_RDONLY);
    assert(fd >= 0 && "Failed to open MSR file (Try running as sudo)");
    uint64_t value;
    int ret = pread(fd, &value, sizeof(value), msr) != sizeof(value);
    assert(ret != 1 && "Failed to read MSR register");
    close(fd);
    return value;
}

int get_socketID (int cpu) {
  char package_id_path[500];
  FILE *fd;
  int socket;
  sprintf (package_id_path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
  fd = fopen (package_id_path, "r");
  assert(fd!=NULL);
  int rc = fscanf (fd, "%d", &socket);
  assert(rc == 1);
  fclose(fd);
  return socket;
}

void gather_machine_info() {
  FILE *lscpu; char buffer[256];
  int cpu_info[4] = {0};
  __asm__ volatile (
      "cpuid"
      : "=b" (cpu_info[0]), "=d" (cpu_info[1]), "=c" (cpu_info[2])
      : "a" (0)
  );
  if (*(int*)&cpu_info[0] == 0x756e6547) {
    PROCESSOR = INTEL;
  } else if (*(int*)&cpu_info[0] == 0x68747541) {
    PROCESSOR = AMD;
  } else {
    assert(0 && "Unknown CPU type");
  }
 
  lscpu = popen("lscpu", "r");
  assert(lscpu != NULL && "Cannot execute lscpu");
 
  while (fgets(buffer, sizeof(buffer), lscpu)) {
    if (strncmp(buffer, "Core(s) per socket:", 19) == 0) {
      sscanf(buffer, "Core(s) per socket: %d", &NUM_PHYSICAL_CORES_PER_SOCKET);
    } else if (strncmp(buffer, "Thread(s) per core:", 19) == 0) {
      sscanf(buffer, "Thread(s) per core: %d" ,&NUM_THREADS_PER_PHYSICAL_CORE);
    } else if (strncmp(buffer, "Socket(s):", 10) == 0) {
      sscanf(buffer, "Socket(s): %d", &NUM_SOCKETS);
    } else if (strncmp(buffer, "CPU(s):", 7) == 0) {
      sscanf(buffer, "CPU(s): %d", &NUM_LOGICAL_CORES);
    }
  }
  pclose(lscpu);
 
  printf("CPU Information:\n");
  printf("Physical cores per socket: %d\n", NUM_PHYSICAL_CORES_PER_SOCKET);
  printf("Threads per core: %d\n", NUM_THREADS_PER_PHYSICAL_CORE);
  printf("Number of sockets: %d\n", NUM_SOCKETS);
  printf("Total logical cores: %d\n", NUM_LOGICAL_CORES);
  assert(NUM_PHYSICAL_CORES_PER_SOCKET>0 && NUM_THREADS_PER_PHYSICAL_CORE>0 && NUM_SOCKETS>0 && "Incorrect machine information");
 
  NUM_CORES = NUM_PHYSICAL_CORES_PER_SOCKET*NUM_SOCKETS;
 
  // Determine first physical core at each socket
  socket_core = (int*) malloc(sizeof(int) * NUM_SOCKETS);
  for(int c=0, curr_socket=-1; c<NUM_CORES; c++) {
    const int sock = get_socketID(c);
    if(sock>curr_socket) {
      curr_socket=sock;
      socket_core[curr_socket]=c;
    }
    if(curr_socket == (NUM_SOCKETS-1)) break;
  }
}


void register_perf_event(int core) {
  struct perf_event_attr attr;
  int ret;
 
  // Initialize the attribute structure
  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(attr);
  attr.config = PERF_COUNT_HW_INSTRUCTIONS;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
 
  printf("Attempting to open perf event for core %d\n", core);
 
  // Try to open the performance event
  instr_fd[core] = perf_event_open(&attr, -1, core, -1, 0);
  if(instr_fd[core] < 0) {
    perror("perf_event_open failed");
    fprintf(stderr, "Error opening perf event for core %d\n", core);
    assert(0 && "Failed to create event");
  }
 
  printf("Successfully opened perf event for core %d\n", core);
 
  // Enable the counter
  ret = ioctl(instr_fd[core], PERF_EVENT_IOC_ENABLE, 0);
  if(ret) {
    perror("ioctl PERF_EVENT_IOC_ENABLE failed");
    fprintf(stderr, "Error enabling counter for core %d\n", core);
    assert(0 && "Failed to enable counter using ioctl");
  }
 
  printf("Successfully enabled counter for core %d\n", core);
}



double read_energy(int record_init, int record_finalize) {
  double diff = 0;
  for(int i=0; i<NUM_SOCKETS; i++) {
    uint64_t energy;
    if(PROCESSOR == AMD) { 
      energy = read_msr(socket_core[i], MSR_PKG_ENERGY_STATUS_AMD);
    } else {
      energy = read_msr(socket_core[i], MSR_PKG_ENERGY_STATUS_INTEL);
    }
    if(record_finalize) {
      diff += energy - energy_start[i];
    } else {
      diff += energy - energy_prev[i];
    }
    energy_prev[i] = energy;
    if(record_init) energy_start[i] = energy;
  }
  return (diff * rapl_joule_unit);
}

uint64_t read_instr() {
  uint64_t diff=0;
  for(int i=0; i<NUM_LOGICAL_CORES; i++) {
    uint64_t values[3] = {0}, val = 0;
    int ret = read(instr_fd[i], values, sizeof(values));
    if (ret != sizeof(values)) assert(0 && "counter reading failed");
    if (values[2]) val = (uint64_t)((double)values[0] * values[1] / values[2]);
    diff += (val - instr_prev[i]);
    instr_prev[i] = val;
  }
  return diff;
}

double calculate_JPI() {
  return (read_energy(0, 0) / ((double) read_instr()));
}

void profiler_init() {
  gather_machine_info();
  uint64_t rapl_power_unit = 0;
  if(PROCESSOR == AMD) { 
    rapl_power_unit = read_msr(0, MSR_RAPL_POWER_UNIT_AMD);
  } else {
    rapl_power_unit = read_msr(0, MSR_RAPL_POWER_UNIT_INTEL);
  }
  assert(rapl_power_unit>0);
  rapl_joule_unit = 1.0 / (1 << ((rapl_power_unit >> 8) & 0x1F)); 
  assert(pfm_initialize() == PFM_SUCCESS && "pfm_initialize Failed");
  instr_fd = (int*) malloc(sizeof(int) * NUM_LOGICAL_CORES);
  instr_prev = (uint64_t*) malloc(sizeof(uint64_t) * NUM_LOGICAL_CORES);
  energy_prev = (uint64_t*) malloc(sizeof(uint64_t) * NUM_SOCKETS);
  energy_start = (uint64_t*) malloc(sizeof(uint64_t) * NUM_SOCKETS);
  for(int i=0; i<NUM_LOGICAL_CORES; i++) {
    register_perf_event(i);
  }
  get_timer(&start_main_timer);
  read_instr();
  read_energy(1, 0);
}

void profiler_finalize() {
  double energy = read_energy(0, 1);
  get_timer(&end_main_timer);
  double time = end_main_timer - start_main_timer;
  fprintf(stdout,"\n============================ Tabulate Statistics ============================\n");
  fprintf(stdout,"TIME(sec)\tENERGY(Joules)\tEDP(Lower-the-Better)\n");
  fprintf(stdout,"%.3f\t%.3f\t%.3f",time, energy, time*energy);
  fprintf(stdout,"\n=============================================================================\n");
  fflush(stdout);
  free(instr_fd);
  free(instr_prev);
  free(energy_prev);
  free(energy_start);
}

namespace quill {

    int num_workers = 1; 
    constexpr size_t DEQUE_SIZE = 50;  
    int N = 6; // Number of workers to sleep

    pthread_t master_thread;
    pthread_mutex_t finish_counter_lock = PTHREAD_MUTEX_INITIALIZER;
    std::queue<int> sleeping_workers_queue;  // Add std:: namespace

    template <size_t DEQUE_SIZE>
    WorkerDeque<DEQUE_SIZE>::WorkerDeque() : head(0), tail(0) {
        pthread_mutex_init(&lock, nullptr); 
        pthread_mutex_init(&counter_lock, nullptr);
    }

    
    template <size_t DEQUE_SIZE>
    void WorkerDeque<DEQUE_SIZE>::push(std::function<void()>*task) {
        // pthread_mutex_lock(&lock);  

        // if (tail < DEQUE_SIZE) {
        //     tasks[tail] = task ;  
        //     tail++;
        // }
        // else {
        //     // cout<<"WorkerDeque overflow: Cannot push, deque is full!"<<endl;
        //     throw std::runtime_error("WorkerDeque overflow: Cannot push, deque is full!");
        // }
        int nextTail = (tail + 1) % DEQUE_SIZE; 

        if (nextTail == head) {  
            std::cerr << "Error: Worker deque is full! Cannot push more tasks." << std::endl;
            std::exit(EXIT_FAILURE); 
        }

        tasks[tail] = task; 
        tail = nextTail; 
            
    }

    
    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::steal(std::function<void()> &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        task = *tasks[head];
        head = (head + 1) % DEQUE_SIZE; 

        pthread_mutex_unlock(&lock);
        return true;
    }


    template <size_t DEQUE_SIZE>
    bool WorkerDeque<DEQUE_SIZE>::pop(std::function<void()> &task) {
        pthread_mutex_lock(&lock);

        if (head == tail) { 
            pthread_mutex_unlock(&lock);
            return false;
        }

        tail = (tail - 1 + DEQUE_SIZE) % DEQUE_SIZE; 
        task = *tasks[tail]; 

        pthread_mutex_unlock(&lock);
        return true;
    }

    std::vector<WorkerDeque<DEQUE_SIZE>> worker_deques;
    std::queue<int> sleeping_workers_queue;
    std::vector<pthread_t> workers;

    volatile bool shutdown = false;
    void init_runtime() {
        profiler_init(); 
        const char* workers_env = std::getenv("QUILL_WORKERS");
        std::cout << "QUILL_WORKERS environment variable: " << (workers_env ? workers_env : "not set") << std::endl;
        
        if (workers_env) {
            try {
                num_workers = std::stoi(workers_env);
                std::cout << "Setting number of workers to: " << num_workers << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error converting QUILL_WORKERS to integer: " << e.what() << std::endl;
                num_workers = 1;
            }
        }
        if (num_workers < 1) {
            num_workers = 1; 
        }
        worker_deques.resize(num_workers);
        workers.resize(num_workers);
        // master_thread = pthread_self(); 
        worker_deques[0].cond = PTHREAD_COND_INITIALIZER; // Initialize the condition variable for the master thread
        for (int i = 1; i < num_workers; ++i) {
            if (pthread_create(&workers[i], nullptr, (void*(*)(void*))worker_func, (void*)(intptr_t)i) != 0) {
                throw std::runtime_error("Failed to create worker thread");
            }
            worker_deques[i].cond = PTHREAD_COND_INITIALIZER;
            // std::cout<<"Worker "<<i<<" created"<<std::endl;
        }
        // create a dedicated pthread for daemon profiler
        pthread_t profiler_thread;
        if (pthread_create(&profiler_thread, nullptr, (void*(*)(void*))daemon_profiler, nullptr) != 0) {
            throw std::runtime_error("Failed to create profiler thread");
        }
        // there should be a queue for the daemon profiler to push worker_ids who are sleeping(counter == 1)
        
        std::cout << "Quill runtime initialized with " << num_workers << " threads." << std::endl;
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
     
        pthread_mutex_lock(&finish_counter_lock);
        finish_counter++;
        pthread_mutex_unlock(&finish_counter_lock);


        std::function<void()>* task_ptr = new std::function<void()>(std::move(lambda));

 
        worker_deques[get_worker_id()].push(task_ptr);
    }


    void find_and_execute_task(int worker_id) {
        // cout << "Worker " << get_worker_id()<< " finding and executing task" << endl;
        // WorkerDeque& deque = worker_deques[worker_id];
        std::function<void()> task;


        if (worker_deques[worker_id].pop(task)) {
            task();  
            // delete &task;  
            pthread_mutex_lock(&finish_counter_lock);
            --finish_counter;
            pthread_mutex_unlock(&finish_counter_lock);  
            task = nullptr;
        } 
        else {
          
            for (int i = 0; i < num_workers; ++i) {
                if (i != worker_id && worker_deques[i].steal(task)) {
                    task();
                    // delete &task;  
                    pthread_mutex_lock(&finish_counter_lock);
                    --finish_counter;
                    pthread_mutex_unlock(&finish_counter_lock);
                    task = nullptr;  
                    return;
                }
            }
            //  check if the counter is 1, if yes then sleep
            pthread_mutex_lock(&worker_deques[worker_id].counter_lock);
            if (worker_deques[worker_id].counter_to_be_used_by_profiler == 1) {
                pthread_mutex_unlock(&worker_deques[worker_id].counter_lock);
                // cout<<"Worker "<<worker_id<<" is sleeping"<<endl;
                pthread_mutex_lock(&worker_deques[worker_id].lock);
                pthread_cond_wait(&worker_deques[worker_id].cond, &worker_deques[worker_id].lock);
                pthread_mutex_unlock(&worker_deques[worker_id].lock);
            } else {
                pthread_mutex_unlock(&worker_deques[worker_id].counter_lock);
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

    void configure_DOP(double JPI_prev, double JPI_curr) {
        // Initialize random number generator
        static std::random_device rd;
        static std::mt19937 gen(rd());
        
        if (JPI_curr == 0) {    // this is the first case
            int count = 0;
            // Create a vector of worker indices and shuffle it
            std::vector<int> worker_indices;
            for (int i = 1; i < num_workers; ++i) {
                worker_indices.push_back(i);
            }
            std::shuffle(worker_indices.begin(), worker_indices.end(), gen);

            for (int i : worker_indices) {
                if (count == N) break;

                pthread_mutex_lock(&worker_deques[i].counter_lock);
                if (worker_deques[i].counter_to_be_used_by_profiler == 1) {
                  pthread_mutex_unlock(&worker_deques[i].counter_lock);
                  continue;
                }

                pthread_mutex_lock(&worker_deques[i].lock);
                if (worker_deques[i].head == worker_deques[i].tail) {
                    worker_deques[i].counter_to_be_used_by_profiler = 1;
                    sleeping_workers_queue.push(i);
                    count++;
                    pthread_mutex_unlock(&worker_deques[i].counter_lock);
                } else {
                    pthread_mutex_unlock(&worker_deques[i].counter_lock);
                }
                pthread_mutex_unlock(&worker_deques[i].lock);
                
            }
        } else {
            // Implement logic to adjust DOP based on JPI values
            // For example, you can increase or decrease the number of threads
            // or adjust the workload distribution based on JPI values
            if (JPI_curr > JPI_prev) {
                // cout << "Increasing DOP" << std::endl;
                // Increase the number of threads or adjust workload
                // to increase, just get N worker ids from queue, and set their counter to be 0
                int count = 0;
                while(!sleeping_workers_queue.empty() && count < N) {
                    int worker_id = sleeping_workers_queue.front();
                    sleeping_workers_queue.pop();
                    pthread_mutex_lock(&worker_deques[worker_id].counter_lock);
                    worker_deques[worker_id].counter_to_be_used_by_profiler = 0;
                    // send pthread_cond_signal to wake up the worker
                    pthread_cond_signal(&worker_deques[worker_id].cond);
                    pthread_mutex_unlock(&worker_deques[worker_id].counter_lock);
                    count++;
                }
            } else {
                // cout << "Decreasing DOP" << std::endl;
                // Decrease the number of threads or adjust workload
                int count = 0;
                std::vector<int> worker_indices;
                for (int i = 1; i < num_workers; ++i) {
                    worker_indices.push_back(i);
                }
                std::shuffle(worker_indices.begin(), worker_indices.end(), gen);

                for (int i : worker_indices) {
                    if (count == N) break;

                    pthread_mutex_lock(&worker_deques[i].counter_lock);
                    if (worker_deques[i].counter_to_be_used_by_profiler == 1) {
                      pthread_mutex_unlock(&worker_deques[i].counter_lock);
                      continue;
                    }

                    pthread_mutex_lock(&worker_deques[i].lock);
                    if (worker_deques[i].head == worker_deques[i].tail) {
                        worker_deques[i].counter_to_be_used_by_profiler = 1;
                        sleeping_workers_queue.push(i);
                        count++;
                        pthread_mutex_unlock(&worker_deques[i].counter_lock);
                        

                    } else {
                        cout<<"Worker "<<i<<" is not sleeping"<<endl;
                        pthread_mutex_unlock(&worker_deques[i].counter_lock);
                    }
                    
                    pthread_mutex_unlock(&worker_deques[i].lock);
                    
                }

            }
        }
    }

    void daemon_profiler() {
        const int fixed_interval = 100; // 100ms interval between measurements
        usleep(100000); // 100ms warmup duration
        double JPI_prev = 0;
        while(!shutdown) {
            double JPI_curr = calculate_JPI();
            configure_DOP(JPI_prev, JPI_curr);
            JPI_prev = JPI_curr;
            usleep(fixed_interval * 1000); // Convert ms to microseconds
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
            // Destroy mutex and condition variable for each worker
            pthread_mutex_destroy(&worker_deques[i].lock);
            pthread_mutex_destroy(&worker_deques[i].counter_lock);
            pthread_cond_destroy(&worker_deques[i].cond);
        }
        // Destroy mutex and condition variable for the master thread
        pthread_mutex_destroy(&worker_deques[0].lock);
        pthread_mutex_destroy(&worker_deques[0].counter_lock);
        pthread_cond_destroy(&worker_deques[0].cond);
        // Destroy the finish counter lock
        pthread_mutex_destroy(&finish_counter_lock);
        profiler_finalize(); 
    }
    
}

















