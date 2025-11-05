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
#include <unordered_map>
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
    // Use unordered_map for O(1) lookup instead of O(log n)
    std::unordered_map<int, IOEventDataId> fd_map;
    
    // Reverse mapping: event_data_id -> fd (for fast fd lookup)
    std::unordered_map<IOEventDataId, int> event_to_fd_map;
    
    // Track poll masks for re-arming
    std::unordered_map<int, uint32_t> poll_mask_map;
    
    // Counter for pending submissions (for batch optimization)
    int pending_submissions;
    
    IOUringContext() : pending_submissions(0) {}
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

// Helper function to get SQE with auto-submit on queue full
static struct io_uring_sqe* get_sqe_with_retry(IOUringContext* ctx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        // Submission queue is full, submit pending operations first
        int ret = io_uring_submit(&ctx->ring);
        if (ret < 0) {
            PLOG(ERROR) << "Failed to submit on SQ full";
            return NULL;
        }
        ctx->pending_submissions = 0;
        
        // Try again
        sqe = io_uring_get_sqe(&ctx->ring);
    }
    return sqe;
}

// Helper function to conditionally submit based on pending count
static void maybe_submit(IOUringContext* ctx, bool force = false) {
    const int BATCH_THRESHOLD = 8;  // Submit when we have 8+ pending operations
    
    // Always submit if there are pending operations and force=true
    // Or submit when threshold is reached
    if (ctx->pending_submissions > 0 && 
        (force || ctx->pending_submissions >= BATCH_THRESHOLD)) {
        int ret = io_uring_submit(&ctx->ring);
        if (ret >= 0) {
            ctx->pending_submissions = 0;
        }
    }
}

int EventDispatcher::RegisterEvent(IOEventDataId event_data_id,
                                   int fd, bool pollin) {
    if (_event_dispatcher_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    // Get a submission queue entry (with auto-retry on full)
    struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
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
    
    // Track this fd and its poll mask (with reverse mapping)
    ctx->fd_map[fd] = event_data_id;
    ctx->event_to_fd_map[event_data_id] = fd;
    ctx->poll_mask_map[fd] = poll_mask;
    ctx->pending_submissions++;
    
    // Batch submit - only submit when threshold reached
    // If threshold not reached, operations will be submitted:
    //   1. When next batch threshold is reached
    //   2. In Run() loop on each iteration (ensures no indefinite delay)
    maybe_submit(ctx, false);
    
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
        struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
        if (!sqe) {
            errno = ENOMEM;
            return -1;
        }
        
        io_uring_prep_poll_add(sqe, fd, POLLIN);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
        
        // Update poll mask
        ctx->poll_mask_map[fd] = POLLIN;
        ctx->pending_submissions++;
        
        maybe_submit(ctx, false);
        return 0;
    } else {
        // Remove poll entirely
        struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
        if (!sqe) {
            errno = ENOMEM;
            return -1;
        }
        
        // Use poll remove operation
        io_uring_prep_poll_remove(sqe, (void*)(uintptr_t)event_data_id);
        
        // Remove from tracking (including reverse mapping)
        ctx->event_to_fd_map.erase(event_data_id);
        ctx->fd_map.erase(fd);
        ctx->poll_mask_map.erase(fd);
        ctx->pending_submissions++;
        
        maybe_submit(ctx, false);
        return 0;
    }
}

int EventDispatcher::AddConsumer(IOEventDataId event_data_id, int fd) {
    if (_event_dispatcher_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
    if (!sqe) {
        errno = ENOMEM;
        LOG(ERROR) << "Failed to get SQE for fd=" << fd;
        return -1;
    }
    
    // Add poll for POLLIN events
    io_uring_prep_poll_add(sqe, fd, POLLIN);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
    
    // Track this fd (with reverse mapping)
    ctx->fd_map[fd] = event_data_id;
    ctx->event_to_fd_map[event_data_id] = fd;
    ctx->poll_mask_map[fd] = POLLIN;
    ctx->pending_submissions++;
    
    maybe_submit(ctx, false);
    
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
    
    struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
    if (!sqe) {
        errno = ENOMEM;
        return -1;
    }
    
    // Remove poll
    io_uring_prep_poll_remove(sqe, (void*)(uintptr_t)event_data_id);
    
    // Remove from tracking (including reverse mapping)
    ctx->event_to_fd_map.erase(event_data_id);
    ctx->fd_map.erase(it);
    ctx->poll_mask_map.erase(fd);
    ctx->pending_submissions++;
    
    maybe_submit(ctx, false);
    
    return 0;
}

void* EventDispatcher::RunThis(void* arg) {
    ((EventDispatcher*)arg)->Run();
    return NULL;
}

// Helper function to re-arm a poll operation
static void rearm_poll(IOUringContext* ctx, int fd, IOEventDataId event_data_id, uint32_t poll_mask) {
    struct io_uring_sqe* sqe = get_sqe_with_retry(ctx);
    if (sqe) {
        io_uring_prep_poll_add(sqe, fd, poll_mask);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
        ctx->pending_submissions++;
    }
}

// Helper function to find fd by event_data_id (O(1) with reverse mapping)
static int find_fd_by_event_data_id(IOUringContext* ctx, IOEventDataId event_data_id) {
    auto it = ctx->event_to_fd_map.find(event_data_id);
    if (it != ctx->event_to_fd_map.end()) {
        return it->second;
    }
    return -1;
}

void EventDispatcher::Run() {
    IOUringContext* ctx = static_cast<IOUringContext*>(_io_uring_ctx);
    
    // Add wakeup fd to io_uring for graceful shutdown
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (sqe) {
        io_uring_prep_poll_add(sqe, _wakeup_fds[0], POLLIN);
        io_uring_sqe_set_data(sqe, NULL); // NULL indicates wakeup fd
        io_uring_submit(&ctx->ring);
        ctx->pending_submissions = 0;
    }
    
    const int BATCH_SIZE = 32;
    struct io_uring_cqe* cqes[BATCH_SIZE];
    
    while (!_stop) {
        // Try to peek multiple CQEs at once (batch processing)
        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe* cqe;
        
        // First, try to get multiple completions without waiting
        io_uring_for_each_cqe(&ctx->ring, head, cqe) {
            cqes[count++] = cqe;
            if (count >= BATCH_SIZE) {
                break;
            }
        }
        
        if (count == 0) {
            // No events ready, wait for at least one
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
            
            cqes[0] = cqe;
            count = 1;
        }
        
        // Process all available completions in batch
        for (unsigned i = 0; i < count; i++) {
            cqe = cqes[i];
            void* user_data = io_uring_cqe_get_data(cqe);
            int32_t res = cqe->res;
            
            // Check if this is the wakeup fd
            if (user_data == NULL) {
                // Consume wakeup data
                char dummy[64];
                ssize_t n = read(_wakeup_fds[0], dummy, sizeof(dummy));
                (void)n;
                
                // Re-arm wakeup fd if not stopping
                if (!_stop) {
                    sqe = get_sqe_with_retry(ctx);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, _wakeup_fds[0], POLLIN);
                        io_uring_sqe_set_data(sqe, NULL);
                        ctx->pending_submissions++;
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
            
            // Find the fd for this event (needed for re-arming)
            int fd = find_fd_by_event_data_id(ctx, event_data_id);
            
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
            
            // Auto re-arm: io_uring poll is one-shot in Linux 5.10
            // Re-register the poll immediately for continuous monitoring
            if (fd >= 0 && !(events & EPOLLHUP)) {
                auto mask_it = ctx->poll_mask_map.find(fd);
                if (mask_it != ctx->poll_mask_map.end()) {
                    uint32_t poll_mask = mask_it->second;
                    rearm_poll(ctx, fd, event_data_id, poll_mask);
                }
            }
        }
        
        // Mark all CQEs as seen in one batch operation
        io_uring_cq_advance(&ctx->ring, count);
        
        // Submit any accumulated operations (re-arms, new polls, etc.)
        // Force submit here to ensure timely re-arming
        maybe_submit(ctx, true);
    }
}

} // namespace brpc
