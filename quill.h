#ifndef QUILL_H
#define QUILL_H
#include <functional>

namespace quill {

    // Asynchronous operation functions
    void async(int top_most, int id, std::function<void()> &&lambda);               // Function for initiating async work
    void start_finish();        // Function to start the finish work
    void end_finish();          // Function to end the finish work
    void parallel_for(int lower, int upper, std::function<void(int, int)> &&body);
    // Runtime initialization and finalization
    void init_runtime();        // Function to initialize the Quill runtime
    void finalize_runtime();    // Function to finalize the Quill runtime
    
}

#endif // QUILL_H
