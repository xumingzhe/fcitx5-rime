#ifndef _FCITX_RIME_ASYNC_QUERY_H_
#define _FCITX_RIME_ASYNC_QUERY_H_
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <fcitx-utils/eventdispatcher.h>
namespace fcitx::rime {
template <typename T, size_t N=64>
class SPSCQueue {
public: bool push(const T &item) { size_t h=head_.load(std::memory_order_relaxed); size_t next=(h+1)%N; if(next==tail_.load(std::memory_order_acquire)) return false; buffer_[h]=item; head_.store(next,std::memory_order_release); return true; }
    bool pop(T &item) { size_t t=tail_.load(std::memory_order_relaxed); if(t==head_.load(std::memory_order_acquire)) return false; item=buffer_[t]; tail_.store((t+1)%N,std::memory_order_release); return true; }
private: T buffer_[N]; std::atomic<size_t> head_{0}; std::atomic<size_t> tail_{0}; };
struct AsyncKeyEvent { uint64_t id; std::string text; int64_t timestamp_ms; std::string query_name; std::string context; };
struct AsyncQueryResult { uint64_t id; std::string query_name; std::vector<std::string> candidates; std::string comment; double quality=0.9; };
struct AsyncQueryConfig { std::string name; int delay_ms=200; std::string comment=" async"; double quality=0.9; std::string worker_script; };
class AsyncManager {
public:
    explicit AsyncManager(fcitx::EventDispatcher &dispatcher);
    ~AsyncManager();
    void registerQuery(const AsyncQueryConfig &config);
    void start(); void stop();
    void onKeyEvent(const std::string &text, const std::string &context = "");
    void onInputChanged();
    template <typename F> int pollResults(F inserter) { int n=0; uint64_t cur=current_id_.load(std::memory_order_acquire); AsyncQueryResult r; while(result_queue_.pop(r)) { if(r.id!=cur) continue; inserter(r); n++; } return n; }
    std::function<void()> onResultReady;
    std::function<void()> insertCallback;
private:
    struct Worker {
        std::string name;
        AsyncQueryConfig config;
        SPSCQueue<AsyncKeyEvent> queue;
        std::thread thread;
        std::atomic<bool> running{false};
    };
    void workerLoop(Worker &w);
    fcitx::EventDispatcher &dispatcher_;
    SPSCQueue<AsyncQueryResult> result_queue_;
    std::mutex result_mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<uint64_t> next_id_{1}, current_id_{0};
    std::string lastInput_;
};
} // namespace fcitx::rime
#endif
