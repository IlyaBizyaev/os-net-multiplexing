#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined (__APPLE__)
#define __BSD__
#endif

#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__BSD__)
#include <sys/event.h>
#else
#error "Only Linux and BSDs/macOS are supported"
#endif

#include <iostream>
#include <string.h>
#include <system_error>

#include "multiplexer.h"


Multiplexer::Multiplexer()
{
    #ifdef __linux__
    m_pollfd = epoll_create1(0); // No flags
    if (m_pollfd == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Failed to create epoll");
    }
    #endif

    #ifdef __BSD__
    m_pollfd = kqueue();
    if (m_pollfd == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Failed to create kqueue");
    }
    #endif
}

Multiplexer::~Multiplexer()
{
    #ifdef __linux__
    if (close(m_pollfd) == -1) {
        std::cerr << "Failed to close poll fd: " << strerror(errno) << std::endl;
    }
    #endif
}

void Multiplexer::add_polled(int fd, int flags) const
{
    #ifdef __linux__
    struct epoll_event event = {};
    event.events = 0;
    if (flags & POLLIN) {
        event.events |= EPOLLIN;
    }
    if (flags & POLLOUT) {
        event.events |= EPOLLOUT;
    }
    if (flags & POLLET) {
        event.events |= EPOLLET;
    }
    event.data.fd = fd;
    if (epoll_ctl(m_pollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Failed to add fd to poll");
    }
    #endif

    #ifdef __BSD__
    struct kevent evset[SUPPORTED_TYPES] = {};
    int kflags = EV_ADD;
    if (flags & POLLET) {
        kflags |= EV_CLEAR;
    }
    EV_SET(&evset[0], fd, EVFILT_READ, kflags | (flags & POLLIN ? EV_ENABLE : EV_DISABLE), 0, 0, NULL);
    EV_SET(&evset[1], fd, EVFILT_WRITE, kflags | (flags & POLLOUT ? EV_ENABLE : EV_DISABLE), 0, 0, NULL);

    if (kevent(m_pollfd, evset, SUPPORTED_TYPES, NULL, 0, NULL) == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Failed to add fd to kqueue");
    }
#endif
}

bool Multiplexer::delete_polled(int fd) const
{
    #ifdef __linux__
    return epoll_ctl(m_pollfd, EPOLL_CTL_DEL, fd, nullptr) != -1;
    #endif

    #ifdef __BSD__
    struct kevent evset[SUPPORTED_TYPES]; // We only support socket r/w
    EV_SET(&evset[0], fd, EVFILT_READ, EV_DISABLE, 0, 0, NULL);
    EV_SET(&evset[1], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, NULL);
    return kevent(m_pollfd, evset, SUPPORTED_TYPES, NULL, 0, NULL) != -1;
    #endif
}

std::vector<Multiplexer::event> Multiplexer::get_ready() const
{
    std::vector<event> ready;

    #ifdef __linux__
    struct epoll_event events[MAX_EVENTS];

    int cnt = epoll_wait(m_pollfd, events, MAX_EVENTS, -1);
    if (cnt == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Event polling failed");
    }

    for (int i = 0; i < cnt; ++i) {
        int fd = events[i].data.fd;

        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            std::cout << "Client disconnected" << std::endl;
            delete_polled(fd);
            close(fd);
        } else {
            Multiplexer::event mevent;
            mevent.fd = fd;

            auto epoll_events = events[i].events;
            if (epoll_events & EPOLLIN) {
                mevent.type |= POLLIN;
            }
            if (epoll_events & EPOLLOUT) {
                mevent.type |= POLLOUT;
            }

            ready.push_back(mevent);
        }
    }
    #endif

    #ifdef __BSD__
    struct kevent events[MAX_EVENTS];

    int cnt = kevent(m_pollfd, NULL, 0, events, MAX_EVENTS, NULL);
    if (cnt == -1) {
        std::error_code ec(errno, std::system_category());
        throw std::system_error(ec, "Event kqueue failed");
    }

    for (int i = 0; i < cnt; ++i) {
        int fd = (int)events[i].ident;

        if (events[i].flags & EV_EOF) {
            std::cout << "Client disconnected" << std::endl;
            close(fd);
            // Automatically removed from kq by the kernel
        } else {
            Multiplexer::event mevent;
            mevent.fd = fd;

            auto kqueue_event = events[i].filter;
            if (kqueue_event == EVFILT_READ) {
                mevent.type = POLLIN;
            } else if (kqueue_event == EVFILT_WRITE) {
                mevent.type = POLLOUT;
            }

            ready.push_back(mevent);
        }
    }
    #endif

    return ready;
}
