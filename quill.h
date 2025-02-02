#ifndef QUILL_H
#define QUILL_H
#include <functional>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace quill {


    void async(std::function<void()> &&lambda);               
    void start_finish();        
    void end_finish();          
    void init_runtime();
    void finalize_runtime();    
    
}

#endif 
