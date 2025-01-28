#ifndef QUILL_H
#define QUILL_H

#include <functional>

namespace quill {

// API to initialize runtime
void init_runtime();

// API to start a finish scope
void start_finish();

// API to end a finish scope
void end_finish();

// API to create an async task
void async(std::function<void()> &&lambda);

// API to finalize runtime and release resources
void finalize_runtime();

} // namespace quill

#endif // QUILL_H
