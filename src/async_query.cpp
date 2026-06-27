#include "async_query.h"
#include <fcitx-utils/log.h>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
namespace fcitx::rime {
using namespace std::chrono;
AsyncManager::AsyncManager(fcitx::EventDispatcher &d) : dispatcher_(d) {}
AsyncManager::~AsyncManager() { stop(); }
void AsyncManager::registerQuery(const AsyncQueryConfig &c) {
    auto w = std::make_unique<Worker>();
    w->name = c.name;
    w->config = c;
    workers_.push_back(std::move(w));
}
void AsyncManager::start() {
    for (auto &w : workers_) {
        if (w->running.exchange(true)) continue;
        w->thread = std::thread(&AsyncManager::workerLoop, this, std::ref(*w));
    }
}
void AsyncManager::stop() {
    for (auto &w : workers_) {
        w->running.store(false);
        if (w->thread.joinable()) w->thread.join();
    }
}
void AsyncManager::onKeyEvent(const std::string &text,
                              const std::string &context) {
    if (text.empty() || text.size() < 4)
        return;
    if (text == lastInput_)
        return;
    lastInput_ = text;
    uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    int64_t ts = duration_cast<milliseconds>(
                     system_clock::now().time_since_epoch())
                     .count();
    for (auto &w : workers_)
        w->queue.push({id, text, ts, w->name, context});
    current_id_.store(id, std::memory_order_release);
}
void AsyncManager::onInputChanged() {
    next_id_.fetch_add(1, std::memory_order_relaxed);
    current_id_.store(next_id_.load(), std::memory_order_release);
}
static std::string doSubprocess(const std::string &script,
                                const std::string &json) {
    int pi[2], po[2];
    if (pipe(pi) || pipe(po))
        return "";
    pid_t pid = fork();
    if (!pid) {
        dup2(pi[0], 0);
        dup2(po[1], 1);
        close(pi[1]);
        close(po[0]);
        execl("/usr/bin/python3", "python3", script.c_str(), (char *)0);
        _exit(1);
    }
    close(pi[0]);
    close(po[1]);
    write(pi[1], json.c_str(), json.size());
    close(pi[1]);
    char buf[16384] = {};
    read(po[0], buf, sizeof(buf) - 1);
    close(po[0]);
    int s;
    waitpid(pid, &s, 0);
    return buf;
}
void AsyncManager::workerLoop(Worker &w) {
    while (w.running.load(std::memory_order_relaxed)) {
        AsyncKeyEvent ev;
        if (w.queue.pop(ev)) {
            int64_t now = duration_cast<milliseconds>(
                              system_clock::now().time_since_epoch())
                              .count();
            int64_t wait = ev.timestamp_ms + w.config.delay_ms - now;
            if (wait > 0)
                std::this_thread::sleep_for(milliseconds(wait));
            if (ev.id != current_id_.load(std::memory_order_acquire))
                continue;
            std::ostringstream req;
            req << "{\"id\":" << ev.id << ",\"text\":\"" << ev.text
                << "\",\"context\":\"" << ev.context
                << "\",\"query\":\"" << ev.query_name << "\"}\n";
            std::string out =
                doSubprocess(w.config.worker_script, req.str());
            if (out.empty())
                continue;
            AsyncQueryResult r;
            r.id = ev.id;
            r.query_name = ev.query_name;
            r.quality = w.config.quality;
            r.comment = w.config.comment;
            size_t pos = 0;
            while (r.candidates.size() < 4) {
                auto p1 = out.find('"', pos);
                if (p1 == std::string::npos)
                    break;
                auto p2 = out.find('"', p1 + 1);
                if (p2 == std::string::npos)
                    break;
                std::string cand = out.substr(p1 + 1, p2 - p1 - 1);
                if (cand.size() >= 2 &&
                    (unsigned char)cand[0] >= 0xe0)
                    r.candidates.push_back(cand);
                pos = p2 + 1;
            }
            if (!r.candidates.empty()) {
                {
                    std::lock_guard<std::mutex> lk(result_mutex_);
                    result_queue_.push(r);
                }
                dispatcher_.schedule([this]() {
                    if (insertCallback)
                        insertCallback();
                });
            }
        } else {
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
}
} // namespace fcitx::rime
