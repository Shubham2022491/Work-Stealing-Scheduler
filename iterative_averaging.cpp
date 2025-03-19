#include<iostream>
#include<algorithm>
#include<stdio.h>
#include<string.h>
#include<cmath>
#include<sys/time.h>
#include "quill.h"
#include <cstdint>


/*
 * Ported from HJlib
 *
 * Author: Vivek Kumar
 *
 */

//48 * 256 * 2048
#define SIZE 25165824
#define ITERATIONS 64

double* myNew, *myVal;
int n;

long get_usecs () {
  struct timeval t;
  gettimeofday(&t,NULL);
  return t.tv_sec*1000000+t.tv_usec;
}
 
int ceilDiv(int d) {
  int m = SIZE / d;
  if (m * d == SIZE) {
    return m;
  } else {
    return (m + 1);
  }
}

void recurse(uint64_t low, uint64_t high) {
  if((high - low) > 512) {
    uint64_t mid = (high+low)/2;
    /* An async task */  
    quill::async(-1,-1,[=]() {
      recurse(low, mid); 
    });
    recurse(mid, high);
  } else {
    for(uint64_t j=low; j<high; j++) {
      myNew[j] = (myVal[j - 1] + myVal[j + 1]) / 2.0;
    }
  }
}

void runParallel() {
  for(int i=0; i<ITERATIONS; i++) {
    // give recurse(1, SIZE+1); to parallel_for
    
    quill::start_finish();
    hclib::start_tracing();
    quill::parallel_for(1, SIZE + 1, [](int start, int end) {
      // This is the body that gets executed for each chunk
      recurse(start, end);
    });
    quill::end_finish();
    hclib::stop_tracing();
    double* temp = myNew;
    myNew = myVal;
    myVal = temp;
  }
}

int main(int argc, char** argv) {
  quill::init_runtime(SIZE);
  myNew = new double[(SIZE + 2)];
  myVal = new double[(SIZE + 2)];
  memset(myNew, 0, sizeof(double) * (SIZE + 2));
  memset(myVal, 0, sizeof(double) * (SIZE + 2));
  myVal[SIZE + 1] = 1.0;
  // quill::init_runtime();
  long start = get_usecs();
  // quill::start_finish();
  runParallel();
  // quill::end_finish();
  long end = get_usecs();
  double dur = ((double)(end-start))/1000000;
  printf("Time = %.3fs\n",dur);
  // quill::finalize_runtime();
  delete(myNew);
  delete(myVal);
  quill::finalize_runtime(SIZE);
}

