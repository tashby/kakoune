#include "event_manager.hh"

#include <unistd.h>

namespace Kakoune
{

FDWatcher::FDWatcher(int fd, Callback callback)
    : m_fd{fd}, m_callback{std::move(callback)}
{
    EventManager::instance().m_fd_watchers.insert(this);
}

FDWatcher::~FDWatcher()
{
    EventManager::instance().m_fd_watchers.erase(this);
}

void FDWatcher::run(EventMode mode)
{
    m_callback(*this, mode);
}

void FDWatcher::close_fd()
{
    close(m_fd);
    m_fd = -1;
}

Timer::Timer(TimePoint date, Callback callback, EventMode mode)
    : m_date{date}, m_callback{std::move(callback)}, m_mode(mode)
{
    if (EventManager::has_instance())
        EventManager::instance().m_timers.insert(this);
}

Timer::~Timer()
{
    if (EventManager::has_instance())
        EventManager::instance().m_timers.erase(this);
}

void Timer::run(EventMode mode)
{
    if (mode == m_mode)
    {
        m_date = TimePoint::max();
        m_callback(*this);
    }
    else // try again a little later
        m_date = Clock::now() + std::chrono::milliseconds{10};
}

EventManager::EventManager()
{
    FD_ZERO(&m_forced_fd);
}

EventManager::~EventManager()
{
    kak_assert(m_fd_watchers.empty());
    kak_assert(m_timers.empty());
}

void EventManager::handle_next_events(EventMode mode)
{
    int max_fd = 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    for (auto& watcher : m_fd_watchers)
    {
        const int fd = watcher->fd();
        if (fd != -1)
        {
            max_fd = std::max(fd, max_fd);
            FD_SET(fd, &rfds);
        }
    }

    timeval tv{};
    if (not m_timers.empty())
    {
        auto next = std::min_element(
            m_timers.begin(), m_timers.end(), [](Timer* lhs, Timer* rhs) {
                return lhs->next_date() < rhs->next_date();
        });
        using namespace std::chrono; using us = std::chrono::microseconds;
        auto usecs = std::max(us(0), duration_cast<us>((*next)->next_date() - Clock::now()));
        auto secs = duration_cast<seconds>(usecs);
        tv = timeval{ (time_t)secs.count(), (suseconds_t)(usecs - secs).count() };
    }
    int res = select(max_fd + 1, &rfds, nullptr, nullptr,
                     m_timers.empty() ? nullptr : &tv);

    // copy forced fds *after* poll, so that signal handlers can write to
    // m_forced_fd, interupt poll, and directly be serviced.
    fd_set forced = m_forced_fd;
    FD_ZERO(&m_forced_fd);

    for (int fd = 0; fd < max_fd + 1; ++fd)
    {
        if ((res > 0 and FD_ISSET(fd, &rfds)) or FD_ISSET(fd, &forced))
        {
            auto it = find_if(m_fd_watchers,
                              [fd](const FDWatcher* w){return w->fd() == fd; });
            if (it != m_fd_watchers.end())
                (*it)->run(mode);
        }
    }

    TimePoint now = Clock::now();
    for (auto& timer : m_timers)
    {
        if (timer->next_date() <= now)
            timer->run(mode);
    }
}

void EventManager::force_signal(int fd)
{
    FD_SET(fd, &m_forced_fd);
}

}
