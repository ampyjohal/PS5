#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace AgcDriver {

namespace {

enum class Mode { Off, High, Realtime };

Mode RequestedMode() {
    static const Mode mode = [] {
        const char* text = std::getenv("APS5_THREAD_PRIORITY");
        if (text == nullptr || *text == '\0' || std::strcmp(text, "off") == 0 || std::strcmp(text, "0") == 0) return Mode::Off;
        if (std::strcmp(text, "rt") == 0 || std::strcmp(text, "realtime") == 0) return Mode::Realtime;
        return Mode::High;
    }();
    return mode;
}

int EnvInt(const char* name, int fallback) {
    const char* text = std::getenv(name);
    return text != nullptr && *text != '\0' ? std::atoi(text) : fallback;
}

void LogOnce(const char* role, const std::string& text) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (!seen.insert(std::string(role) + "|" + text).second) return;
    std::fprintf(stderr, "[thread-priority] %s: %s\n", role, text.c_str());
}

#ifndef _WIN32

struct DBusConnection;
struct DBusMessage;
struct DBusError {
    const char* name;
    const char* message;
    unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
    void* padding1;
};
constexpr int DbusBusSystem = 1;
constexpr int DbusTypeInvalid = 0;
constexpr int DbusTypeInt32 = static_cast<int>('i');
constexpr int DbusTypeUint32 = static_cast<int>('u');
constexpr int DbusTypeUint64 = static_cast<int>('t');

struct Dbus {
    void* handle = nullptr;
    DBusConnection* connection = nullptr;
    int (*threadsInitDefault)() = nullptr;
    void (*errorInit)(DBusError*) = nullptr;
    void (*errorFree)(DBusError*) = nullptr;
    int (*errorIsSet)(const DBusError*) = nullptr;
    DBusConnection* (*busGet)(int, DBusError*) = nullptr;
    DBusMessage* (*messageNewMethodCall)(const char*, const char*, const char*, const char*) = nullptr;
    int (*messageAppendArgs)(DBusMessage*, int, ...) = nullptr;
    DBusMessage* (*sendWithReplyAndBlock)(DBusConnection*, DBusMessage*, int, DBusError*) = nullptr;
    void (*messageUnref)(DBusMessage*) = nullptr;
    std::string failure;
};

Dbus& LoadDbus() {
    static Dbus dbus = [] {
        Dbus loaded;
        loaded.handle = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (loaded.handle == nullptr) {
            loaded.failure = "libdbus-1.so.3 is not available";
            return loaded;
        }
        const auto resolve = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(loaded.handle, name));
            if (function == nullptr && loaded.failure.empty()) loaded.failure = std::string("libdbus lacks ") + name;
        };
        resolve(loaded.threadsInitDefault, "dbus_threads_init_default");
        resolve(loaded.errorInit, "dbus_error_init");
        resolve(loaded.errorFree, "dbus_error_free");
        resolve(loaded.errorIsSet, "dbus_error_is_set");
        resolve(loaded.busGet, "dbus_bus_get");
        resolve(loaded.messageNewMethodCall, "dbus_message_new_method_call");
        resolve(loaded.messageAppendArgs, "dbus_message_append_args");
        resolve(loaded.sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block");
        resolve(loaded.messageUnref, "dbus_message_unref");
        if (!loaded.failure.empty()) return loaded;
        loaded.threadsInitDefault();
        DBusError error{};
        loaded.errorInit(&error);
        loaded.connection = loaded.busGet(DbusBusSystem, &error);
        if (loaded.connection == nullptr) {
            loaded.failure = std::string("system bus: ") + (loaded.errorIsSet(&error) && error.message != nullptr ? error.message : "unavailable");
            loaded.errorFree(&error);
        }
        return loaded;
    }();
    return dbus;
}

bool RtkitCall(const char* method, std::uint64_t tid, int type, const void* value, std::string& failure) {
    auto& dbus = LoadDbus();
    if (dbus.connection == nullptr) {
        failure = dbus.failure;
        return false;
    }
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto* message = dbus.messageNewMethodCall("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", method);
    if (message == nullptr) {
        failure = "dbus message allocation failed";
        return false;
    }
    const std::uint64_t pid = static_cast<std::uint64_t>(getpid());
    bool ok = false;
    if (type == DbusTypeInt32) ok = dbus.messageAppendArgs(message, DbusTypeUint64, &pid, DbusTypeUint64, &tid, DbusTypeInt32, value, DbusTypeInvalid) != 0;
    else ok = dbus.messageAppendArgs(message, DbusTypeUint64, &pid, DbusTypeUint64, &tid, DbusTypeUint32, value, DbusTypeInvalid) != 0;
    if (!ok) {
        dbus.messageUnref(message);
        failure = "dbus argument append failed";
        return false;
    }
    DBusError error{};
    dbus.errorInit(&error);
    auto* reply = dbus.sendWithReplyAndBlock(dbus.connection, message, 5000, &error);
    dbus.messageUnref(message);
    if (reply == nullptr) {
        failure = std::string(method) + ": " + (dbus.errorIsSet(&error) && error.message != nullptr ? error.message : "no reply");
        dbus.errorFree(&error);
        return false;
    }
    dbus.messageUnref(reply);
    return true;
}

bool PrepareRealtime(std::string& failure) {
    rlimit limit{};
    if (getrlimit(RLIMIT_RTTIME, &limit) != 0) {
        failure = "getrlimit(RLIMIT_RTTIME) failed";
        return false;
    }
    const rlim_t maxUs = static_cast<rlim_t>(EnvInt("APS5_THREAD_RTTIME_US", 200000));
    limit.rlim_max = maxUs;
    limit.rlim_cur = maxUs / 2;
    if (setrlimit(RLIMIT_RTTIME, &limit) != 0) {
        failure = "setrlimit(RLIMIT_RTTIME) failed";
        return false;
    }
    sched_param param{};
    if (sched_getparam(0, &param) != 0 || sched_setscheduler(0, sched_getscheduler(0) | SCHED_RESET_ON_FORK, &param) != 0) {
        failure = "SCHED_RESET_ON_FORK could not be set";
        return false;
    }
    return true;
}

void Raise(const char* role) {
    const auto tid = static_cast<std::uint64_t>(syscall(SYS_gettid));
    std::string failure;
    if (RequestedMode() == Mode::Realtime) {
        const std::uint32_t priority = static_cast<std::uint32_t>(std::max(1, std::min(99, EnvInt("APS5_THREAD_RT", 10))));
        if (PrepareRealtime(failure) && RtkitCall("MakeThreadRealtimeWithPID", tid, DbusTypeUint32, &priority, failure)) {
            LogOnce(role, "SCHED_FIFO " + std::to_string(priority) + " via rtkit");
            return;
        }
        LogOnce(role, "realtime refused (" + failure + "), trying a nice level");
    }
    const std::int32_t nice = static_cast<std::int32_t>(std::max(-20, std::min(19, EnvInt("APS5_THREAD_NICE", -10))));
    if (RtkitCall("MakeThreadHighPriorityWithPID", tid, DbusTypeInt32, &nice, failure)) {
        LogOnce(role, "nice " + std::to_string(nice) + " via rtkit");
        return;
    }
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0) {
        LogOnce(role, "nice " + std::to_string(nice) + " (setpriority)");
        return;
    }
    LogOnce(role, "not raised: " + failure + "; setpriority refused too (RLIMIT_NICE)");
}

#else

void Raise(const char* role) {
    const int priority = RequestedMode() == Mode::Realtime ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_HIGHEST;
    if (SetThreadPriority(GetCurrentThread(), priority)) LogOnce(role, priority == THREAD_PRIORITY_TIME_CRITICAL ? "time-critical" : "highest");
    else LogOnce(role, "SetThreadPriority failed");
}

#endif

}

void RaiseWorkerThreadPriority(const char* role) {
    if (RequestedMode() == Mode::Off) return;
    Raise(role);
}

}
