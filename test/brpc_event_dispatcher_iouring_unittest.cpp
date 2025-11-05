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

// Unit tests for io_uring event dispatcher

#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/fd_utility.h"
#include "bthread/bthread.h"
#include "brpc/event_dispatcher.h"
#include "brpc/socket.h"

#ifdef BRPC_ENABLE_IO_URING
#include <liburing.h>

namespace {

class IOUringEventDispatcherTest : public ::testing::Test {
protected:
    IOUringEventDispatcherTest() = default;
    ~IOUringEventDispatcherTest() override = default;
    
    void SetUp() override {
        // Check if io_uring is available on this system
        struct io_uring ring;
        int ret = io_uring_queue_init(2, &ring, 0);
        if (ret < 0) {
            GTEST_SKIP() << "io_uring not available on this system (kernel < 5.10 or not enabled)";
        }
        io_uring_queue_exit(&ring);
    }
    
    void TearDown() override {}
};

// Test basic io_uring availability check
TEST_F(IOUringEventDispatcherTest, io_uring_availability) {
    struct io_uring ring;
    int ret = io_uring_queue_init(256, &ring, 0);
    ASSERT_GE(ret, 0) << "Failed to initialize io_uring: " << strerror(-ret);
    
    // Verify ring fd is valid
    ASSERT_GT(ring.ring_fd, 0);
    
    io_uring_queue_exit(&ring);
}

// Test io_uring queue initialization with different sizes
TEST_F(IOUringEventDispatcherTest, queue_init_sizes) {
    struct io_uring ring;
    
    // Test small queue
    int ret = io_uring_queue_init(2, &ring, 0);
    ASSERT_GE(ret, 0);
    io_uring_queue_exit(&ring);
    
    // Test medium queue
    ret = io_uring_queue_init(64, &ring, 0);
    ASSERT_GE(ret, 0);
    io_uring_queue_exit(&ring);
    
    // Test large queue
    ret = io_uring_queue_init(256, &ring, 0);
    ASSERT_GE(ret, 0);
    io_uring_queue_exit(&ring);
}

// Test io_uring poll add operation
TEST_F(IOUringEventDispatcherTest, poll_add) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add poll for read event
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x1234);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring poll with actual event
TEST_F(IOUringEventDispatcherTest, poll_with_event) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add poll for read event
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x5678);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Write data to trigger the poll event
    char data = 'X';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    // Wait for completion
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    ASSERT_NE(nullptr, cqe);
    
    // Verify the event
    void* user_data = io_uring_cqe_get_data(cqe);
    ASSERT_EQ((void*)0x5678, user_data);
    ASSERT_GT(cqe->res, 0); // Should have POLLIN set
    
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring poll remove operation
TEST_F(IOUringEventDispatcherTest, poll_remove) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add poll
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    void* user_data = (void*)0x9999;
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, user_data);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Remove the poll
    sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_remove(sqe, user_data);
    io_uring_sqe_set_data(sqe, (void*)0xAAAA);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Should get two completions: one for cancel, one for remove operation
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    io_uring_cqe_seen(&ring, cqe);
    
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring with multiple fds
TEST_F(IOUringEventDispatcherTest, multiple_fds) {
    const int NUM_PIPES = 5;
    int fds[NUM_PIPES][2];
    
    for (int i = 0; i < NUM_PIPES; ++i) {
        ASSERT_EQ(0, pipe(fds[i]));
    }
    
    struct io_uring ring;
    int ret = io_uring_queue_init(64, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add polls for all pipes
    for (int i = 0; i < NUM_PIPES; ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        ASSERT_NE(nullptr, sqe);
        
        io_uring_prep_poll_add(sqe, fds[i][0], POLLIN);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)(i + 1));
    }
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(NUM_PIPES, ret);
    
    // Write to all pipes
    for (int i = 0; i < NUM_PIPES; ++i) {
        char data = 'A' + i;
        ASSERT_EQ(1, write(fds[i][1], &data, 1));
    }
    
    // Collect all completions
    int completed = 0;
    bool seen[NUM_PIPES] = {false};
    
    while (completed < NUM_PIPES) {
        struct io_uring_cqe* cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        ASSERT_GE(ret, 0);
        ASSERT_NE(nullptr, cqe);
        
        uintptr_t idx = (uintptr_t)io_uring_cqe_get_data(cqe) - 1;
        ASSERT_LT(idx, NUM_PIPES);
        ASSERT_FALSE(seen[idx]);
        seen[idx] = true;
        
        io_uring_cqe_seen(&ring, cqe);
        completed++;
    }
    
    // Verify all events received
    for (int i = 0; i < NUM_PIPES; ++i) {
        ASSERT_TRUE(seen[i]);
    }
    
    io_uring_queue_exit(&ring);
    for (int i = 0; i < NUM_PIPES; ++i) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
}

// Test io_uring timeout behavior
TEST_F(IOUringEventDispatcherTest, timeout) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add poll without writing data
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x1111);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Add a timeout
    sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    struct __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000; // 100ms
    
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data(sqe, (void*)0x2222);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    butil::Timer timer;
    timer.start();
    
    // Wait for completion (should timeout)
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    
    timer.stop();
    
    // Should complete in approximately 100ms
    ASSERT_GE(timer.m_elapsed(), 50); // At least 50ms
    ASSERT_LE(timer.m_elapsed(), 200); // At most 200ms
    
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring with edge-triggered behavior
TEST_F(IOUringEventDispatcherTest, edge_triggered_behavior) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Write data before adding poll
    char data[] = "test data";
    ASSERT_EQ(sizeof(data), write(fds[1], data, sizeof(data)));
    
    // Add poll (should trigger immediately since data is available)
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x3333);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Should get completion immediately
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    ASSERT_NE(nullptr, cqe);
    ASSERT_GT(cqe->res, 0);
    
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring with closed fd
TEST_F(IOUringEventDispatcherTest, closed_fd) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Add poll
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x4444);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Close the fd
    close(fds[0]);
    close(fds[1]);
    
    // Should get completion with error or POLLHUP
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    
    // Either error or POLLHUP is acceptable
    if (ret >= 0 && cqe != nullptr) {
        io_uring_cqe_seen(&ring, cqe);
    }
    
    io_uring_queue_exit(&ring);
}

// Test io_uring POLLIN and POLLOUT events
TEST_F(IOUringEventDispatcherTest, pollin_pollout_events) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Test POLLOUT (socket should be writable immediately)
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLOUT);
    io_uring_sqe_set_data(sqe, (void*)0x5555);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Should complete immediately
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    ASSERT_NE(nullptr, cqe);
    ASSERT_GT(cqe->res, 0);
    ASSERT_TRUE(cqe->res & POLLOUT);
    
    io_uring_cqe_seen(&ring, cqe);
    
    // Test POLLIN
    sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, fds[0], POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x6666);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Write data
    char data = 'Y';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    // Should get POLLIN event
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    ASSERT_NE(nullptr, cqe);
    ASSERT_GT(cqe->res, 0);
    ASSERT_TRUE(cqe->res & POLLIN);
    
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

// Test io_uring with invalid fd
TEST_F(IOUringEventDispatcherTest, invalid_fd) {
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    // Try to poll an invalid fd
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(nullptr, sqe);
    
    io_uring_prep_poll_add(sqe, -1, POLLIN);
    io_uring_sqe_set_data(sqe, (void*)0x7777);
    
    ret = io_uring_submit(&ring);
    ASSERT_EQ(1, ret);
    
    // Should get error completion
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    ASSERT_GE(ret, 0);
    ASSERT_NE(nullptr, cqe);
    ASSERT_LT(cqe->res, 0); // Should be error
    
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
}

// Test io_uring queue overflow handling
TEST_F(IOUringEventDispatcherTest, queue_overflow) {
    struct io_uring ring;
    const int QUEUE_SIZE = 8;
    int ret = io_uring_queue_init(QUEUE_SIZE, &ring, 0);
    ASSERT_GE(ret, 0);
    
    int pipes[QUEUE_SIZE * 2][2];
    for (int i = 0; i < QUEUE_SIZE * 2; ++i) {
        ASSERT_EQ(0, pipe(pipes[i]));
    }
    
    // Try to add more entries than queue size
    int added = 0;
    for (int i = 0; i < QUEUE_SIZE; ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, pipes[i][0], POLLIN);
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)(i + 1));
            added++;
        }
    }
    
    ASSERT_GT(added, 0);
    ret = io_uring_submit(&ring);
    ASSERT_EQ(added, ret);
    
    io_uring_queue_exit(&ring);
    for (int i = 0; i < QUEUE_SIZE * 2; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

// Test io_uring re-arming poll after event
TEST_F(IOUringEventDispatcherTest, rearm_poll) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    ASSERT_GE(ret, 0);
    
    const int NUM_EVENTS = 3;
    for (int i = 0; i < NUM_EVENTS; ++i) {
        // Add poll
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        ASSERT_NE(nullptr, sqe);
        
        io_uring_prep_poll_add(sqe, fds[0], POLLIN);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)(i + 1));
        
        ret = io_uring_submit(&ring);
        ASSERT_EQ(1, ret);
        
        // Write data
        char data = 'A' + i;
        ASSERT_EQ(1, write(fds[1], &data, 1));
        
        // Wait for event
        struct io_uring_cqe* cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        ASSERT_GE(ret, 0);
        ASSERT_NE(nullptr, cqe);
        
        uintptr_t event_id = (uintptr_t)io_uring_cqe_get_data(cqe);
        ASSERT_EQ(i + 1, event_id);
        
        io_uring_cqe_seen(&ring, cqe);
        
        // Read the data
        char buf;
        ASSERT_EQ(1, read(fds[0], &buf, 1));
        ASSERT_EQ('A' + i, buf);
    }
    
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
}

} // namespace

#else // !BRPC_ENABLE_IO_URING

// Placeholder test when io_uring is not enabled
TEST(IOUringEventDispatcherTest, not_enabled) {
    GTEST_SKIP() << "io_uring support not enabled (compile with -DWITH_IO_URING=ON)";
}

#endif // BRPC_ENABLE_IO_URING


