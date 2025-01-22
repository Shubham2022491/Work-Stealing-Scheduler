#ifndef QUILL_H
#define QUILL_H



// Initialize the runtime system
void init_runtime();

// Finalize the runtime system
void finalize_runtime();

// Start a finish scope
void start_finish();

// End a finish scope
void end_finish();

// Launch a task asynchronously
void async(void (*task)(void*), void* args);

#endif // QUILL_H
