/**
 * @file scheduler.cpp
 * @author zhangyuhang(eicfnb@163.com)
 * @brief 
 * @version 0.1
 * @date 2023-12-18
 */
#include "scheduler.h"
#include "macro.h"
#include "hook.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;
/// 当前线程的调度协程，每个线程都独有一份
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
    SYLAR_ASSERT(threads > 0);

    m_useCaller = use_caller;
    m_name      = name;

    if (use_caller) {
        --threads;
        /**
         * 如果caller线程也用作线程池中的一个的话 要在caller申请两个协程，主协程和scheduler协程，因为caller协程可能要去做别的工作，不能一直把主协程作为scheduler，不然caller线程没法做别的工作了
         * 所以这里先设置主协程
         */
        sylar::Fiber::GetThis();
        SYLAR_ASSERT(GetThis() == nullptr);
        t_scheduler = this;

        /**
         * caller线程的主协程不会被线程的调度协程run进行调度，而且，线程的调度协程停止时，应该返回caller线程的主协程
         * 在user caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
         * 然后这里设置调度协程 注意这里设置了false 表明caller的调度协程swapcontext的时候 会调到主协程
         */
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        sylar::Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread      = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler *Scheduler::GetThis() {
    return t_scheduler;
}

Fiber *Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

void Scheduler::setThis() {
    t_scheduler = this;
}

Scheduler::~Scheduler() {
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
    SYLAR_ASSERT(m_stopping);
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

/**
 * @brief 开始调度协程，这里开启了除了本线程之外所有线程的调度，如果本线程也是调度器线程池中的一个，那本线程的调度开启需要等待调用stop stop中调用run
 * 
 */
void Scheduler::start() {
    SYLAR_LOG_DEBUG(g_logger) << "start";
    MutexType::Lock lock(m_mutex);
    if (m_stopping) {
        SYLAR_LOG_ERROR(g_logger) << "Scheduler is stopped";
        return;
    }
    SYLAR_ASSERT(m_threads.empty());
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; i++) {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                      m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
}

/**
 * @brief 已经调用了stop 并且没有要调度的任务了 也没有正在执行任务的线程
 * 
 * @return true 
 * @return false 
 */
bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

/**
 * @brief 没有实际用处 因为run目前是忙等循环
 * 
 */
void Scheduler::tickle() {
    SYLAR_LOG_DEBUG(g_logger) << "tickle";
}

/**
 * @brief 没有任务时候的闲逛协程 如果没有停止 直接回去 其实基本就是进一下就走
 * 
 */
void Scheduler::idle() {
    SYLAR_LOG_DEBUG(g_logger) << "idle";
    while (!stopping()) {
        sylar::Fiber::GetThis()->yield();
    }
}
/**
 * @brief caller线程开启本线程的调度和停止，如果caller线程不参与调度，那就是纯粹的结束调度器的调度
 * 
 */
void Scheduler::stop() {
    SYLAR_LOG_DEBUG(g_logger) << "stop";
    if (stopping()) {
        return;
    }
    /// stop直接修改m_stopping 其余两个需要等待fiber调用完成
    m_stopping = true;

    /// 如果use caller，那只能由caller线程发起stop
    if (m_useCaller) {
        SYLAR_ASSERT(GetThis() == this);
    } else {
        /**
         * ????
         * 如果不用usecaller的话，只能由线程池外的线程发起stop
         * 可以是caller线程或者其他线程（除了线程池中的）
         */
        SYLAR_ASSERT(GetThis() != this);
    }
    /**
     * 再次触发一遍所有的调度，以便完成未完成的fiber
     * 
     */
    for (size_t i = 0; i < m_threadCount; i++) {
        tickle();
    }
    /**
     * 如果caller线程也用来~，也触发一次
     * 
     */
    if (m_rootFiber) {
        tickle();
    }

    /// 在use caller情况下，调度器协程结束时，应该返回caller线程的主协程
    if (m_rootFiber) {
        m_rootFiber->resume();
        SYLAR_LOG_DEBUG(g_logger) << "m_rootFiber end";
    }

    /// 最后结束线程
    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }
    for (auto &i : thrs) {
        i->join();
    }
}
/**
 * @brief 调度器正式开启，可能是线程池中新的线程调用/或者caller线程调用了stop，然后stop给了caller线程的调度协程，caller线程的调度协程调用
 * 
 */
void Scheduler::run() {
    SYLAR_LOG_DEBUG(g_logger) << "run";
    set_hook_enable(true);
    /**
     * 首先设置本线程的scheduler
     * 
     */
    setThis();
    /**
     * 如果现在这个调度器的调度不是caller线程的，那么把调度协程设置为主协程即可，caller线程的调度协程不可以是主协程
     * 总之到这里两个thread local变量都设置好了
     */
    if (sylar::GetThreadId() != m_rootThread) {
        t_scheduler_fiber = sylar::Fiber::GetThis().get();
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while (true) {
        task.reset();
        bool tickle_me = false; // 是否tickle其他线程进行任务调度
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有调度任务
            while (it != m_tasks.end()) {
                if (it->thread != -1 && it->thread != sylar::GetThreadId()) {
                    // 指定了调度线程，但不是在当前线程上调度，标记一下需要通知其他线程进行调度，然后跳过这个任务，继续下一个
                    ++it;
                    tickle_me = true;
                    continue;
                }

                // 找到一个未指定线程，或是指定了当前线程的任务
                SYLAR_ASSERT(it->fiber || it->cb);
                if (it->fiber) {
                    // 任务队列时的协程一定是READY状态，谁会把RUNNING或TERM状态的协程加入调度呢？
                    SYLAR_ASSERT(it->fiber->getState() == Fiber::READY);
                }
                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1，调度到协程的时候才算是active
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            tickle_me |= (it != m_tasks.end());
        }

        if (tickle_me) {
            tickle();
        }

        if (task.fiber) {
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减一
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        } else if (task.cb) {
            if (cb_fiber) {
                cb_fiber->reset(task.cb);
            } else {
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } else {
            // 进到这个分支情况一定是任务队列空了，调度idle协程即可
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                SYLAR_LOG_DEBUG(g_logger) << "idle fiber term";
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
}

} // end namespace sylar