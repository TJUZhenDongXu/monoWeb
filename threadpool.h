#ifndef WEBSERVER_THREADPOOL_H_     
#define WEBSERVER_THREADPOOL_H_     

#include <vector>
#include <queue>


class ThreadPool {
public:
    explicit ThreadPool(std::size_t n = 8);
    ~ThreadPool();
    void AddTask(std::function<void()> task);
private:
    void Worker();
    std::vector<std::thread>  workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                mtx_;
    std::condition_variable   cv_;
    bool                      stop_{false};
};


#endif
