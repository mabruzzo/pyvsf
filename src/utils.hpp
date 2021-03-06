#ifndef UTILS_H
#define UTILS_H

#include <cstdio>
#include <execinfo.h>

[[noreturn]] inline void error(const char* message){
  if (message == nullptr){
    std::printf("ERROR\n");
  } else {
    std::printf("ERROR: %s\n", message);
  }

  std::printf("\nPrinting backtrace:\n");
  void* callstack_arr[256];
  int n_frames = backtrace(callstack_arr, 256);
  char** strings = backtrace_symbols(callstack_arr, n_frames);
  for (int i = 0; i < n_frames; ++i) {
    std::printf("%s\n", strings[i]);
  }
  std::free(strings);

  exit(1);
}

#endif /* UTILS_H */
