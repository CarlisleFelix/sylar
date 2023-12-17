/**
 * @file util.cpp
 * @author zhangyuhang(eicfnb@163.com)
 * @brief 
 * @version 0.1
 * @date 2023-12-15
 */
#include "util.h"
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace sylar {

pid_t GetThreadId() {
    return syscall(SYS_gettid);
}

uint64_t GetFiberId() {
    return 0;
}

uint64_t GetElapsed() {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000.0;
}

std::string GetThreadName() {
    char thread_name[16] = {0};
    pthread_getname_np(pthread_self(), thread_name, 16);
    return std::string(thread_name);
}

void SetThreadName(const std::string &name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

} // namespace sylar
