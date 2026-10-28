#include "Session.h"

#include <glib.h>

#include <chrono>

namespace nppremote {

namespace {

struct MainCall {
    std::shared_ptr<std::atomic<bool>> alive;
    std::function<void()> fn;
};

gboolean runMainCall(gpointer data)
{
    auto* call = static_cast<MainCall*>(data);
    if (*call->alive)
        call->fn();
    delete call;
    return G_SOURCE_REMOVE;
}

} // namespace

Session::Session(const Profile& profile, Hooks hooks)
    : profile_(profile), hooks_(std::move(hooks)), alive_(std::make_shared<std::atomic<bool>>(true))
{
    ClientHooks ch;
    ch.log = [this](LogKind k, const std::string& s) {
        toMain([this, k, s] {
            if (hooks_.log)
                hooks_.log(k, s);
        });
    };
    ch.askSecret = [this](const std::string& prompt, std::string& answer) {
        std::string got;
        const bool ok = callMain<bool>([&] { return hooks_.askSecret && hooks_.askSecret(prompt, got); }, false);
        answer = got;
        return ok;
    };
    ch.confirm = [this](const std::string& question) {
        return callMain<bool>([&] { return hooks_.confirm && hooks_.confirm(question); }, false);
    };
    client_ = makeClient(profile_, ch);
    thread_ = std::thread([this] { loop(); });
}

Session::~Session()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    *alive_ = false; // nothing more reaches the GTK side
    cv_.notify_all();
    if (running_) // idle: the worker logs out politely
        client_->abort();
    if (thread_.joinable())
        thread_.join();
}

void Session::toMain(std::function<void()> fn)
{
    g_idle_add(runMainCall, new MainCall{alive_, std::move(fn)});
}

template <typename R>
R Session::callMain(std::function<R()> fn, R onStop)
{
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        R result{};
    };
    auto shared = std::make_shared<Shared>();
    toMain([shared, fn] {
        R r = fn();
        std::lock_guard<std::mutex> lock(shared->m);
        shared->result = r;
        shared->done = true;
        shared->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(shared->m);
    while (!shared->done) {
        if (stopping_)
            return onStop;
        shared->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    return shared->result;
}

void Session::post(Work work, Done done)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({std::move(work), std::move(done)});
    }
    cv_.notify_all();
}

void Session::cancel()
{
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped.swap(queue_);
        cancelled_ = true;
    }
    client_->abort();
    for (Job& j : dropped)
        if (j.done)
            j.done(false, "cancelled");
}

void Session::reportConnection()
{
    const bool now = client_->connected();
    if (now == reportedConnected_)
        return;
    reportedConnected_ = now;
    toMain([this, now] {
        if (hooks_.connectionChanged)
            hooks_.connectionChanged(now);
    });
}

bool Session::ensureConnected()
{
    if (client_->connected())
        return true;
    const bool ok = client_->connect();
    reportConnection();
    return ok;
}

void Session::loop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                break;
            job = std::move(queue_.front());
            queue_.pop_front();
            cancelled_ = false;
            running_ = true;
        }
        toMain([this] {
            if (hooks_.busyChanged)
                hooks_.busyChanged(true);
        });
        bool ok = ensureConnected() && job.work(*client_);
        if (!ok && client_->connectionLost() && !stopping_ && !cancelled_) {
            // Idle servers drop the connection: once more, on a new one.
            toMain([this] {
                if (hooks_.log)
                    hooks_.log(LogKind::Info, "Connection lost, reconnecting");
            });
            ok = ensureConnected() && job.work(*client_);
        }
        reportConnection();
        const std::string error = ok ? std::string() : cancelled_ ? std::string("cancelled") : client_->error();
        bool idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idle = queue_.empty();
            running_ = false;
        }
        toMain([this, done = std::move(job.done), ok, error, idle] {
            if (idle && hooks_.busyChanged)
                hooks_.busyChanged(false);
            // Last: `done` may end the session (and this object).
            if (done)
                done(ok, error);
        });
    }
    client_->disconnect();
}

} // namespace nppremote
