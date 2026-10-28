// NppRemote — one connection and its worker thread. The GTK thread queues
// jobs; each runs on the worker with a connected client (connecting, and
// reconnecting once when the server dropped the line), and its result comes
// back on the GTK thread. The user is asked for passwords and host keys on
// the GTK thread while the worker waits.
#pragma once

#include "RemoteClient.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace nppremote {

class Session {
public:
    // Called on the GTK thread.
    struct Hooks {
        std::function<void(LogKind, const std::string&)> log;
        std::function<bool(const std::string& prompt, std::string& answer)> askSecret;
        std::function<bool(const std::string& question)> confirm;
        std::function<void(bool connected)> connectionChanged;
        std::function<void(bool busy)> busyChanged;
    };
    using Work = std::function<bool(RemoteClient&)>;
    using Done = std::function<void(bool ok, const std::string& error)>;

    Session(const Profile& profile, Hooks hooks);
    ~Session(); // cancels what is running and logs out

    const Profile& profile() const { return profile_; }
    // `work` runs on the worker; `done` (optional) on the GTK thread.
    void post(Work work, Done done = {});
    // Stops the running job and drops the queued ones (their `done` gets
    // "cancelled"). The next job connects again.
    void cancel();

    // Runs `fn` on the GTK thread later, unless the session is gone by then.
    void toMain(std::function<void()> fn);

private:
    struct Job {
        Work work;
        Done done;
    };
    void loop();
    bool ensureConnected();
    template <typename R>
    R callMain(std::function<R()> fn, R onStop);
    void reportConnection();

    Profile profile_;
    Hooks hooks_;
    std::unique_ptr<RemoteClient> client_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    bool reportedConnected_ = false; // worker side
    std::thread thread_;
};

} // namespace nppremote
