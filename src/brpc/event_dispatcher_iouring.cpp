// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#include <liburing.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <map>
#include "butil/fd_utility.h"
#include "butil/logging.h"

namespace brpc {

// Check if io_uring is available on current kernel
static bool is_io_uring_available() {
    // Try to create a minimal io_uring instance to test availability
    struct io_uring ring;
    
    // Use io_uring_queue_init which internally calls io_uring_setup
    // This tests if kernel supports io_uring properly
    int ret = io_uring_queue_init(2, &ring, 0);
    if (ret < 0) {
        // io_uring not available
        VLOG(1) << "io_uring not available: " << strerror(-ret);
        return false;
    }
    
    // Clean up the test ring
    io_uring_queue_exit(&ring);
    
    VLOG(1) << "io_uring is available and functional";
    return true;
}

// Wrapper structure to hold io_uring instance and fd tracking
struct IOUringContext {
    struct io_uring ring;
    
    // Track file descriptors and their associated event data
    // Key: fd, Value: IOEventDataId
    std::map<int, IOEventDataId> fd_map;
    
    IOUringContext() {}
    ~IOUringContext() {}
};

EventDispatcher::EventDispatcher()
    : _event_dispatcher_fd(-1)
    , _stop(false)
    , _tid(0)
    , _thread_attr(BTHREAD_ATTR_NORMAL) {
    
    // Check if io_uring is available
    if (!is_io_uring_available()) {
        LOG(WARNING) << "io_uring not available, please check kernel version (need >= 5.10)";
        return;
    }
    
    IOUringContext* ctx = new (std::nothrow) IOUringContext();
    if (!ctx) {
        LOG(ERROR) << "Failed to allocate IOUringContext";
        return;
    }
    
    // Initialize io_uring with queue depth of 256
    // This is a good default for most workloads
    int ret = io_uring_queue_init(256, &ctx->ring, 0);
    if (ret < 0) {
        delete ctx;
        PLOG(ERROR) << "Failed to initialize io_uring: " << strerror(-ret);
        return;
    }
    
    // Get the ring file descriptor for use in Stop()
    _event_dispatcher_fd = ctx->ring.ring_fd;
    _io_uring_ctx = ctx;
    
    _wakeup_fds[0] = -1;
    _wakeup_fds[1] = -1;
    if (pipe(_wakeup_fds) != 0) {
        PLOG(FATAL) << "Fail to create pipe";
        return;
    }
    CHECK_EQ(0, butil::make_close_on_exec(_wakeup_fds[0]));
    CHECK_EQ(0, butil::make_close_on_exec(_wakeup_fds[1]));
    
    LOG(INFO) << "io_uring EventDispatcher initialized successfully";
}

EventDispatcher::~EventDispatcher() {
    Stop();
    Join();
    
    if (_io_uring_ctx) {
        IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
        io_uring_queue_exit(&ctx->ring);
        delete ctx;
        _io_uring_ctx = NULL;
    }
    
    _event_dispatcher_fd = -1;
    
    if (_wakeup_fds[0] > 0) {
        close(_wakeup_fds[0]);
        close(_wakeup_fds[1]);
    }
}

int EventDispatcher::Start(const bthread_attr_t* thread_attr) {
    if (_event_dispatcher_fd < 0) {
        LOG(FATAL) << "io_uring was not created";
        return -1;
    }
    
    if (_tid != 0) {
        LOG(FATAL) << "Already started this dispatcher(" << this 
                   << ") in bthread=" << _tid;
        return -1;
    }

    if (thread_attr) {
        _thread_attr = *thread_attr;
    }

    bthread_attr_t iouring_thread_attr =
        _thread_attr | BTHREAD_NEVER_QUIT | BTHREAD_GLOBAL_PRIORITY;

    int rc = bthread_start_background(&_tid, &iouring_thread_attr, RunThis, this);
    if (rc) {
        LOG(FATAL) << "Fail to create io_uring thread: " << berror(rc);
        return -1;
    }
    return 0;
}

bool EventDispatcher::Running() const {
    return !_stop && _event_dispatcher_fd >= 0 && _tid != 0;
}

void EventDispatcher::Stop() {
    _stop = true;
    
    // Wake up the io_uring thread by writing to pipe
    if (_wakeup_fds[1] >= 0) {
        char dummy = 'W';
        ssize_t n = write(_wakeup_fds[1], &dummy, 1);
        (void)n; // Suppress unused warning
    }
}

void EventDispatcher::Join() {
    if (_tid) {
        bthread_join(_tid, NULL);
        _tid = 0;
    }
}

int EventDispatcher::RegisterEvent(IOEventDataId event_data_id,
                                   int fd, bool pollin) {
    if (_event_dispatcher_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    // Get a submission queue entry
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = ENOMEM;
        LOG(ERROR) << "Failed to get SQE for fd=" << fd;
        return -1;
    }
    
    // Prepare poll add operation
    uint32_t poll_mask = POLLOUT;
    if (pollin) {
        poll_mask |= POLLIN;
    }
    
    io_uring_prep_poll_add(sqe, fd, poll_mask);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
    
    // Track this fd
    ctx->fd_map[fd] = event_data_id;
    
    // Submit the operation
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        PLOG(ERROR) << "Failed to submit poll add for fd=" << fd;
        return -1;
    }
    
    return 0;
}

int EventDispatcher::UnregisterEvent(IOEventDataId event_data_id,
                                     int fd, bool pollin) {
    if (_event_dispatcher_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    if (pollin) {
        // Re-register with only POLLIN
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            errno = ENOMEM;
            return -1;
        }
        
        io_uring_prep_poll_add(sqe, fd, POLLIN);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
        
        int ret = io_uring_submit(&ctx->ring);
        if (ret < 0) {
            PLOG(ERROR) << "Failed to re-submit poll for fd=" << fd;
            return -1;
        }
        return 0;
    } else {
        // Remove poll entirely
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            errno = ENOMEM;
            return -1;
        }
        
        // Use poll remove operation
        io_uring_prep_poll_remove(sqe, (void*)(uintptr_t)event_data_id);
        
        // Remove from tracking
        ctx->fd_map.erase(fd);
        
        int ret = io_uring_submit(&ctx->ring);
        if (ret < 0) {
            PLOG(ERROR) << "Failed to submit poll remove for fd=" << fd;
            return -1;
        }
        return 0;
    }
}

int EventDispatcher::AddConsumer(IOEventDataId event_data_id, int fd) {
    if (_event_dispatcher_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = ENOMEM;
        LOG(ERROR) << "Failed to get SQE for fd=" << fd;
        return -1;
    }
    
    // Add poll for POLLIN events
    io_uring_prep_poll_add(sqe, fd, POLLIN);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
    
    // Track this fd
    ctx->fd_map[fd] = event_data_id;
    
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        PLOG(ERROR) << "Failed to submit poll add for fd=" << fd;
        return -1;
    }
    
    return 0;
}

int EventDispatcher::RemoveConsumer(int fd) {
    if (fd < 0 || _event_dispatcher_fd < 0) {
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    // Find the event data id
    auto it = ctx->fd_map.find(fd);
    if (it == ctx->fd_map.end()) {
        // Not tracked, just return success
        return 0;
    }
    
    IOEventDataId event_data_id = it->second;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = ENOMEM;
        return -1;
    }
    
    // Remove poll
    io_uring_prep_poll_remove(sqe, (void*)(uintptr_t)event_data_id);
    
    // Remove from tracking
    ctx->fd_map.erase(it);
    
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        PLOG(WARNING) << "Failed to submit poll remove for fd=" << fd;
        // Don't return error as the fd might already be closed
    }
    
    return 0;
}

void* EventDispatcher::RunThis(void* arg) {
    ((EventDispatcher*)arg)->Run();
    return NULL;
}

void EventDispatcher::Run() {
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    // Add wakeup fd to io_uring for graceful shutdown
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (sqe) {
        io_uring_prep_poll_add(sqe, _wakeup_fds[0], POLLIN);
        io_uring_sqe_set_data(sqe, NULL); // NULL indicates wakeup fd
        io_uring_submit(&ctx->ring);
    }
    
    while (!_stop) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion event
        int ret = io_uring_wait_cqe(&ctx->ring, &cqe);
        
        if (_stop) {
            break;
        }
        
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            PLOG(ERROR) << "io_uring_wait_cqe failed";
            break;
        }
        
        if (!cqe) {
            continue;
        }
        
        // Get user data
        void* user_data = io_uring_cqe_get_data(cqe);
        int32_t res = cqe->res;
        
        // Mark CQE as seen
        io_uring_cqe_seen(&ctx->ring, cqe);
        
        // Check if this is the wakeup fd
        if (user_data == NULL) {
            // Consume wakeup data
            char dummy[64];
            ssize_t n = read(_wakeup_fds[0], dummy, sizeof(dummy));
            (void)n;
            
            // Re-arm wakeup fd if not stopping
            if (!_stop) {
                sqe = io_uring_get_sqe(&ctx->ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, _wakeup_fds[0], POLLIN);
                    io_uring_sqe_set_data(sqe, NULL);
                    io_uring_submit(&ctx->ring);
                }
            }
            continue;
        }
        
        // Handle I/O event
        if (res < 0) {
            // Error occurred
            if (res != -ECANCELED) {
                VLOG(1) << "io_uring poll returned error: " << strerror(-res);
            }
            continue;
        }
        
        IOEventDataId event_data_id = (IOEventDataId)(uintptr_t)user_data;
        uint32_t revents = res;
        
        // Convert poll events to epoll-style events
        uint32_t events = 0;
        if (revents & POLLIN) {
            events |= EPOLLIN;
        }
        if (revents & POLLOUT) {
            events |= EPOLLOUT;
        }
        if (revents & POLLERR) {
            events |= EPOLLERR;
        }
        if (revents & POLLHUP) {
            events |= EPOLLHUP;
        }
        
        // Call input callback if readable
        if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
            int64_t start_ns = butil::cpuwide_time_ns();
            CallInputEventCallback(event_data_id, events, _thread_attr);
            (*g_edisp_read_lantency) << (butil::cpuwide_time_ns() - start_ns);
        }
        
        // Call output callback if writable
        if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            int64_t start_ns = butil::cpuwide_time_ns();
            CallOutputEventCallback(event_data_id, events, _thread_attr);
            (*g_edisp_write_lantency) << (butil::cpuwide_time_ns() - start_ns);
        }
        
        // Note: With io_uring, poll operations are one-shot by default
        // The application needs to re-arm the poll if continuous monitoring is needed
        // For brpc's use case, the socket code will re-register as needed
    }
}

} // namespace brpc
