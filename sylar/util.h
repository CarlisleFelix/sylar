/**
 * @file util.h
 * @author zhangyuhang(eicfnb@163.com)
 * @brief 
 * @version 0.1
 * @date 2023-12-15
 */
#ifndef __SYLAR_UTIL_H__
#define __SYLAR_UTIL_H__

#include <stdint.h>
#include <sys/types.h>

namespace sylar {

/**
 * @brief 获取线程id
 * @note 这里不要把pid_t和pthread_t混淆，参考man 2 gettid
 */
pid_t GetThreadId();

/**
 * @brief 获取协程id
 * @attention 桩函数，暂时返回0，等协程模块完善后再返回实际值
 */
uint64_t GetFiberId();

} // namespace sylar

#endif // __SYLAR_UTIL_H__