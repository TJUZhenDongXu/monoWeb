#include "epoller.h"
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

Epoller::Epoller(int max_events) : max_events_(max_events), events_(max_events) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
}

Epoller::~Epoller() { 
    close(epoll_fd_); 
}

bool Epoller::addFd(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events  = events;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("EPOLL_CTL_ADD");
        return false;
    }
    return true;
}

bool Epoller::modFd(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events  = events;
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0){
        perror("EPOLL_CTL_MOD");
        return false;
    }
    return true;
}

bool Epoller::delFd(int fd){
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        perror("epoll_ctl DEL");
        return false;
    }
    return true;
}

int Epoller::wait(int timeout_ms){
    int nfds = epoll_wait(epoll_fd_, events_.data(), max_events_, timeout_ms);
    if(nfds < 0 && errno!= EINTR){
        perror("epoll wait");
    }
    return nfds;
}

epoll_event* Epoller::getEvents(){
    return events_.data();
}