#include <vector>
#include <memory>
#include <future>
#include <thread>
#include <type_traits>

#include "threadsafequeue.h"
#include "lockfreeworkstealingqueue.h"
#include "threadwrapper.h"

// for debug
#include <iostream>

class ThreadPool {
private:
    using TaskType = FunctionWrapper; 

    /**
     * members guaranteed to be initialized by order of 
     * declaration and destroyed in reverse order
     */
    std::atomic<bool> done;
    std::atomic<unsigned int> cur_queues_size;
    // Note: ensure that queues and pool_queue are destroyed 
    // AFTER threads join
    std::vector<std::unique_ptr<LockFreeWorkStealingQueue> > queues;
    ThreadSafeQueue<TaskType> pool_queue;
    // these two must come after all queues
    std::vector<std::thread> threads;
    ThreadWrapper joiner;

    static thread_local LockFreeWorkStealingQueue* local_queue;
    static thread_local unsigned int idx;

    void worker_thread(unsigned int idx_) {
        idx = idx_;
        local_queue = queues[idx].get();
        // std::cout << "thread-" << idx << " inside worker_thread.\n";
        while (!done) {
            // std::cout << "thread-" << idx << " before running pending task.\n";
            run_pending_task();
        }
    }

    bool pop_task_from_local_queue(TaskType& task) {
        // std::cout << "thread-" << idx << " inside pop_task_from_local_queue\n";
        return local_queue && local_queue->try_pop_back(task);
    }

    bool pop_task_from_pool_queue(TaskType& task) {
        // std::cout << "thread-" << idx << " inside pop_task_from_pool_queue\n";
        return pool_queue.try_pop(task);
    }

    bool pop_task_from_other_thread_queue(TaskType& task) {
        // std::cout << "thread-" << idx << " inside pop_task_from_other_thread_queue\n";
        auto sz = cur_queues_size.load(std::memory_order_acquire);
        for (auto i = 0; i < sz; ++i) {
            const unsigned int index = (idx+1+i) % sz;
            if (queues[index] && queues[index]->try_steal_front(task)) {
                return true;
            }
        }
        return false;
    }

public:
    ThreadPool() : done(false), joiner(threads) {
        cur_queues_size.store(0, std::memory_order_relaxed);
        const unsigned int thread_count = std::thread::hardware_concurrency();

        try {
            queues.reserve(thread_count);
            threads.reserve(thread_count);
            for (unsigned int i = 0; i < thread_count; ++i) {
                queues.push_back(std::make_unique<LockFreeWorkStealingQueue>());
                cur_queues_size.fetch_add(1, std::memory_order_release);
                threads.push_back(std::thread(&ThreadPool::worker_thread, this, i));
                std::cout << "thread: " << i << " was created.\n";
            }
        } catch (...) {
            std::cout << "exception catched.\n";
            done = true;
            throw;
        }
    }

    ~ThreadPool() {
        done = true;

        // explicitly call dtor in order to make sure
        // they are destroyed in correct order
        // joiner.~ThreadWrapper();
        // threads.~vector<std::thread>();
        // pool_queue.~ThreadSafeQueue();
        // queues.~vector<std::unique_ptr<LockFreeWorkStealingQueue>>();
    }

    template <typename FunctionType, typename... Args>
    std::future<std::result_of_t<FunctionType(Args...)> > submit(FunctionType f, Args... args) {
        using result_type = std::result_of_t<FunctionType(Args...)>;
        std::packaged_task<result_type(Args...)> task(std::forward<FunctionType>(f));
        std::future<result_type> res(task.get_future());

        if (local_queue) {
            local_queue->push_back(FunctionWrapper(std::move(task), std::forward<Args>(args)...));
        } else {
            // std::cout << "pushing to pool_queue******************\n";
            pool_queue.push(FunctionWrapper(std::move(task), std::forward<Args>(args)...));
        }

        return res;
    }

    void run_pending_task() {
        // std::cout << "thread-" << idx << " inside run_pending_task.\n";
        TaskType task;
        if (pop_task_from_local_queue(task) || pop_task_from_pool_queue(task) ||
                pop_task_from_other_thread_queue(task)) {
            // std::cout << "thread-" << idx << " invoke task().\n";
            task();
        } else {
            // std::cout << "thread-" << idx << " no task to run, yield.\n";
            std::this_thread::yield();
        }
    }
};
