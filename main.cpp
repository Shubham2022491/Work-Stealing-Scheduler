#include <iostream>
#include "quill.h"           // Include the core async functions
// #include "quill-runtime.h"    // Include the runtime-specific functions

int main() {
    try {
        // Initialize the Quill runtime
        quill::init_runtime();

        // // Perform some asynchronous work
        quill::start_finish(); // Start a finish operation
        quill::async([]() { std::cout << "Hello from async!" << std::endl; }); 
        quill::end_finish();   // End the finish operation

        // // Finalize the Quill runtime
        quill::finalize_runtime();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
