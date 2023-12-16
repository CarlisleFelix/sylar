/**
 * @file util.cpp
 * @author zhangyuhang(eicfnb@163.com)
 * @brief 
 * @version 0.1
 * @date 2023-12-15
 */
#include "util.h"
#include <sys/syscall.h>
#include <unistd.h>

namespace sylar {

pid_t GetThreadId() {
    return syscall(SYS_gettid);
}

uint64_t GetFiberId() {
    return 0;
}
} // namespace sylar
