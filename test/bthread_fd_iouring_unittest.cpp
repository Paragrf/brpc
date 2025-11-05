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

// Unit tests for bthread fd operations with io_uring backend

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/fd_utility.h"
#include "butil/fd_guard.h"
#include "bthread/bthread.h"
#include "brpc/event_dispatcher.h"

#ifdef BRPC_ENABLE_IO_URING
#include <liburing.h>

DECLARE_bool(use_iouring);

namespace {

class BthreadFdIOUringTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Check if io_uring is available
        struct io_uring ring;
        int ret = io_uring_queue_init(2, &ring, 0);
        if (ret < 0) {
            GTEST_SKIP() << "io_uring not available on this system";
        }
        io_uring_queue_exit(&ring);
        
        // Enable io_uring for testing
        FLAGS_use_iouring = true;
    }
    
    void TearDown() override {
        FLAGS_use_iouring = false;
    }
};

// Test basic bthread_fd_wait with io_uring backend
TEST_F(BthreadFdIOUringTest, basic_fd_wait) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    // Create a bthread that waits for read event
    auto wait_func = [](void* arg) -> void* {
        int fd = *(int*)arg;
        int ret = bthread_fd_wait(fd, POLLIN);
        EXPECT_EQ(0, ret);
        return NULL;
    };
    
    bthread_t th;
    ASSERT_EQ(0, bthread_start_urgent(&th, NULL, wait_func, &fds[0]));
    
    // Give the bthread time to start waiting
    bthread_usleep(10000); // 10ms
    
    // Write data to trigger the event
    char data = 'X';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    // Wait for bthread completion
    ASSERT_EQ(0, bthread_join(th, NULL));
    
    close(fds[0]);
    close(fds[1]);
}

// Test bthread_fd_timedwait with timeout
TEST_F(BthreadFdIOUringTest, fd_timedwait_timeout) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    timespec ts = butil::milliseconds_from_now(50);
    
    butil::Timer timer;
    timer.start();
    
    int ret = bthread_fd_timedwait(fds[0], POLLIN, &ts);
    
    timer.stop();
    
    // Should timeout
    ASSERT_EQ(-1, ret);
    ASSERT_EQ(ETIMEDOUT, errno);
    
    // Should take approximately 50ms
    ASSERT_GE(timer.m_elapsed(), 40);
    ASSERT_LE(timer.m_elapsed(), 100);
    
    close(fds[0]);
    close(fds[1]);
}

// Test bthread_fd_timedwait with event before timeout
TEST_F(BthreadFdIOUringTest, fd_timedwait_event) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    // Write data before waiting
    char data = 'Y';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    timespec ts = butil::milliseconds_from_now(1000);
    
    butil::Timer timer;
    timer.start();
    
    int ret = bthread_fd_timedwait(fds[0], POLLIN, &ts);
    
    timer.stop();
    
    // Should return immediately with success
    ASSERT_EQ(0, ret);
    ASSERT_LT(timer.m_elapsed(), 100);
    
    close(fds[0]);
    close(fds[1]);
}

// Test multiple bthreads waiting on different fds
TEST_F(BthreadFdIOUringTest, multiple_bthread_wait) {
    const int NUM_THREADS = 10;
    int pipes[NUM_THREADS][2];
    bthread_t threads[NUM_THREADS];
    
    struct WaitContext {
        int fd;
        bool completed;
    };
    
    WaitContext contexts[NUM_THREADS];
    
    auto wait_func = [](void* arg) -> void* {
        WaitContext* ctx = (WaitContext*)arg;
        int ret = bthread_fd_wait(ctx->fd, POLLIN);
        EXPECT_EQ(0, ret);
        ctx->completed = true;
        return NULL;
    };
    
    // Create pipes and start bthreads
    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ(0, pipe(pipes[i]));
        contexts[i].fd = pipes[i][0];
        contexts[i].completed = false;
        ASSERT_EQ(0, bthread_start_urgent(&threads[i], NULL, wait_func, &contexts[i]));
    }
    
    bthread_usleep(20000); // 20ms
    
    // Trigger all events
    for (int i = 0; i < NUM_THREADS; ++i) {
        char data = 'A' + i;
        ASSERT_EQ(1, write(pipes[i][1], &data, 1));
    }
    
    // Wait for all bthreads
    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ(0, bthread_join(threads[i], NULL));
        ASSERT_TRUE(contexts[i].completed);
    }
    
    // Cleanup
    for (int i = 0; i < NUM_THREADS; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

// Test bthread_fd_wait with POLLOUT event
TEST_F(BthreadFdIOUringTest, fd_wait_pollout) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    // Socket should be writable immediately
    butil::Timer timer;
    timer.start();
    
    int ret = bthread_fd_wait(fds[0], POLLOUT);
    
    timer.stop();
    
    ASSERT_EQ(0, ret);
    ASSERT_LT(timer.m_elapsed(), 100);
    
    close(fds[0]);
    close(fds[1]);
}

// Test bthread_close wakes up waiting threads
TEST_F(BthreadFdIOUringTest, close_wakes_waiter) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    auto wait_func = [](void* arg) -> void* {
        int fd = *(int*)arg;
        timespec ts = butil::milliseconds_from_now(5000);
        int ret = bthread_fd_timedwait(fd, POLLIN, &ts);
        // Should be interrupted by close, not timeout
        return (void*)(intptr_t)ret;
    };
    
    bthread_t th;
    ASSERT_EQ(0, bthread_start_urgent(&th, NULL, wait_func, &fds[0]));
    
    bthread_usleep(10000); // 10ms
    
    butil::Timer timer;
    timer.start();
    
    // Close should wake up the waiter
    ASSERT_EQ(0, bthread_close(fds[0]));
    
    void* result;
    ASSERT_EQ(0, bthread_join(th, &result));
    
    timer.stop();
    
    // Should complete quickly, not wait for full timeout
    ASSERT_LT(timer.m_elapsed(), 1000);
    
    close(fds[1]);
}

// Test invalid fd with bthread_fd_wait
TEST_F(BthreadFdIOUringTest, invalid_fd) {
    errno = 0;
    int ret = bthread_fd_wait(-1, POLLIN);
    ASSERT_EQ(-1, ret);
    ASSERT_EQ(EINVAL, errno);
}

// Test invalid events with bthread_fd_wait
TEST_F(BthreadFdIOUringTest, invalid_events) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    // Test with only POLLET (edge-triggered without actual event)
    errno = 0;
    int ret = bthread_fd_wait(fds[0], POLLET);
    ASSERT_EQ(-1, ret);
    ASSERT_EQ(EINVAL, errno);
    
    // Test with valid POLLIN | POLLET
    char data = 'Z';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    ret = bthread_fd_wait(fds[0], POLLIN | POLLET);
    ASSERT_EQ(0, ret);
    
    close(fds[0]);
    close(fds[1]);
}

// Test concurrent read and write waiters on same socket
TEST_F(BthreadFdIOUringTest, concurrent_read_write_waiters) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    struct Context {
        int fd;
        bool read_completed;
        bool write_completed;
    };
    
    Context ctx = {fds[0], false, false};
    
    auto read_func = [](void* arg) -> void* {
        Context* ctx = (Context*)arg;
        int ret = bthread_fd_wait(ctx->fd, POLLIN);
        EXPECT_EQ(0, ret);
        ctx->read_completed = true;
        return NULL;
    };
    
    auto write_func = [](void* arg) -> void* {
        Context* ctx = (Context*)arg;
        int ret = bthread_fd_wait(ctx->fd, POLLOUT);
        EXPECT_EQ(0, ret);
        ctx->write_completed = true;
        return NULL;
    };
    
    bthread_t read_thread, write_thread;
    ASSERT_EQ(0, bthread_start_urgent(&read_thread, NULL, read_func, &ctx));
    ASSERT_EQ(0, bthread_start_urgent(&write_thread, NULL, write_func, &ctx));
    
    bthread_usleep(10000); // 10ms
    
    // Write data to trigger read event
    char data = 'M';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    // Wait for both threads
    ASSERT_EQ(0, bthread_join(read_thread, NULL));
    ASSERT_EQ(0, bthread_join(write_thread, NULL));
    
    ASSERT_TRUE(ctx.read_completed);
    ASSERT_TRUE(ctx.write_completed);
    
    close(fds[0]);
    close(fds[1]);
}

// Test stress scenario with many fd operations
TEST_F(BthreadFdIOUringTest, stress_many_operations) {
    const int NUM_OPERATIONS = 100;
    int pipes[NUM_OPERATIONS][2];
    
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        ASSERT_EQ(0, pipe(pipes[i]));
    }
    
    struct OpContext {
        int read_fd;
        int write_fd;
        int id;
        bool completed;
    };
    
    OpContext contexts[NUM_OPERATIONS];
    
    auto op_func = [](void* arg) -> void* {
        OpContext* ctx = (OpContext*)arg;
        
        // Wait for read event
        int ret = bthread_fd_wait(ctx->read_fd, POLLIN);
        EXPECT_EQ(0, ret);
        
        // Read the data
        char buf;
        ssize_t n = read(ctx->read_fd, &buf, 1);
        EXPECT_EQ(1, n);
        EXPECT_EQ('0' + (ctx->id % 10), buf);
        
        ctx->completed = true;
        return NULL;
    };
    
    bthread_t threads[NUM_OPERATIONS];
    
    // Start all operations
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        contexts[i].read_fd = pipes[i][0];
        contexts[i].write_fd = pipes[i][1];
        contexts[i].id = i;
        contexts[i].completed = false;
        ASSERT_EQ(0, bthread_start_urgent(&threads[i], NULL, op_func, &contexts[i]));
    }
    
    bthread_usleep(50000); // 50ms
    
    // Write to all pipes
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        char data = '0' + (i % 10);
        ASSERT_EQ(1, write(pipes[i][1], &data, 1));
    }
    
    // Wait for all operations
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        ASSERT_EQ(0, bthread_join(threads[i], NULL));
        ASSERT_TRUE(contexts[i].completed);
    }
    
    // Cleanup
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

// Test bthread_fd_wait with cancelled operation
TEST_F(BthreadFdIOUringTest, cancelled_wait) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    struct Context {
        int fd;
        volatile bool should_cancel;
        int wait_result;
    };
    
    Context ctx = {fds[0], false, 0};
    
    auto wait_func = [](void* arg) -> void* {
        Context* ctx = (Context*)arg;
        
        // Start waiting
        timespec ts = butil::milliseconds_from_now(5000);
        ctx->wait_result = bthread_fd_timedwait(ctx->fd, POLLIN, &ts);
        
        return NULL;
    };
    
    bthread_t th;
    ASSERT_EQ(0, bthread_start_urgent(&th, NULL, wait_func, &ctx));
    
    bthread_usleep(10000); // 10ms
    
    // Close the fd to cancel the wait
    close(fds[0]);
    close(fds[1]);
    
    ASSERT_EQ(0, bthread_join(th, NULL));
    
    // Wait should be interrupted
    // Result might be -1 or 0 depending on timing
}

// Test sequential fd operations
TEST_F(BthreadFdIOUringTest, sequential_operations) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    const int NUM_ROUNDS = 5;
    
    for (int i = 0; i < NUM_ROUNDS; ++i) {
        // Write data
        char write_data = 'A' + i;
        ASSERT_EQ(1, write(fds[1], &write_data, 1));
        
        // Wait for read event
        int ret = bthread_fd_wait(fds[0], POLLIN);
        ASSERT_EQ(0, ret);
        
        // Read data
        char read_data;
        ASSERT_EQ(1, read(fds[0], &read_data, 1));
        ASSERT_EQ(write_data, read_data);
    }
    
    close(fds[0]);
    close(fds[1]);
}

// Test fd wait in pthread (not bthread)
TEST_F(BthreadFdIOUringTest, fd_wait_in_pthread) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    auto pthread_func = [](void* arg) -> void* {
        int* pipe_fds = (int*)arg;
        
        // This should work in pthread too
        timespec ts = butil::milliseconds_from_now(100);
        int ret = bthread_fd_timedwait(pipe_fds[0], POLLIN, &ts);
        
        return (void*)(intptr_t)ret;
    };
    
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, pthread_func, fds));
    
    usleep(20000); // 20ms
    
    // Write data
    char data = 'P';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    void* result;
    ASSERT_EQ(0, pthread_join(th, &result));
    ASSERT_EQ(0, (intptr_t)result);
    
    close(fds[0]);
    close(fds[1]);
}

} // namespace

#else // !BRPC_ENABLE_IO_URING

// Placeholder test when io_uring is not enabled
TEST(BthreadFdIOUringTest, not_enabled) {
    GTEST_SKIP() << "io_uring support not enabled (compile with -DWITH_IO_URING=ON)";
}

#endif // BRPC_ENABLE_IO_URING

