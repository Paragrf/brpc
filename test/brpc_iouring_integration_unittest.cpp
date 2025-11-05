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

// Integration tests for io_uring mode with existing brpc components

#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/fd_utility.h"
#include "butil/endpoint.h"
#include "bthread/bthread.h"
#include "brpc/socket.h"
#include "brpc/acceptor.h"
#include "brpc/server.h"
#include "brpc/channel.h"

#ifdef BRPC_ENABLE_IO_URING
#include <liburing.h>

DECLARE_bool(use_iouring);

namespace bthread {
    extern TaskControl* g_task_control;
}

namespace {

// Base test fixture for io_uring integration tests
class IOUringIntegrationTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        // Check if io_uring is available
        struct io_uring ring;
        int ret = io_uring_queue_init(2, &ring, 0);
        if (ret < 0) {
            GTEST_SKIP() << "io_uring not available on this system";
        }
        io_uring_queue_exit(&ring);
        
        // Set io_uring mode based on test parameter
        bool use_iouring = GetParam();
        FLAGS_use_iouring = use_iouring;
        
        if (use_iouring) {
            LOG(INFO) << "Running test with io_uring mode enabled";
        } else {
            LOG(INFO) << "Running test with epoll mode (for comparison)";
        }
    }
    
    void TearDown() override {
        FLAGS_use_iouring = false;
    }
};

// Test bthread concurrency calculation with io_uring
TEST_P(IOUringIntegrationTest, bthread_concurrency) {
    bool use_iouring = GetParam();
    
    // Get initial concurrency
    int concurrency = bthread_getconcurrency();
    LOG(INFO) << "Initial concurrency: " << concurrency 
              << " (mode: " << (use_iouring ? "io_uring" : "epoll") << ")";
    
    // Default is 8 + BTHREAD_EPOLL_THREAD_NUM (which is 1)
    // With io_uring, the thread model might be different
    ASSERT_GT(concurrency, 0);
    
    // Test setting concurrency
    int new_concurrency = concurrency + 2;
    ASSERT_EQ(0, bthread_setconcurrency(new_concurrency));
    ASSERT_EQ(new_concurrency, bthread_getconcurrency());
    
    // Create some bthreads to verify they work
    auto dummy_func = [](void*) -> void* { return NULL; };
    
    std::vector<bthread_t> threads;
    for (int i = 0; i < 10; ++i) {
        bthread_t tid;
        ASSERT_EQ(0, bthread_start_urgent(&tid, NULL, dummy_func, NULL));
        threads.push_back(tid);
    }
    
    for (auto tid : threads) {
        ASSERT_EQ(0, bthread_join(tid, NULL));
    }
}

// Test socket operations with io_uring
TEST_P(IOUringIntegrationTest, socket_operations) {
    bool use_iouring = GetParam();
    
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    auto write_func = [](void* arg) -> void* {
        int fd = *(int*)arg;
        bthread_usleep(10000); // 10ms
        
        const char* msg = "Hello from bthread";
        ssize_t n = write(fd, msg, strlen(msg));
        EXPECT_GT(n, 0);
        return NULL;
    };
    
    auto read_func = [](void* arg) -> void* {
        int fd = *(int*)arg;
        
        // Wait for data
        int ret = bthread_fd_wait(fd, POLLIN);
        EXPECT_EQ(0, ret);
        
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        EXPECT_GT(n, 0);
        buf[n] = '\0';
        
        EXPECT_STREQ("Hello from bthread", buf);
        return NULL;
    };
    
    bthread_t write_tid, read_tid;
    ASSERT_EQ(0, bthread_start_urgent(&write_tid, NULL, write_func, &fds[1]));
    ASSERT_EQ(0, bthread_start_urgent(&read_tid, NULL, read_func, &fds[0]));
    
    ASSERT_EQ(0, bthread_join(write_tid, NULL));
    ASSERT_EQ(0, bthread_join(read_tid, NULL));
    
    close(fds[0]);
    close(fds[1]);
}

// Test multiple concurrent socket operations
TEST_P(IOUringIntegrationTest, concurrent_socket_operations) {
    bool use_iouring = GetParam();
    
    const int NUM_PAIRS = 20;
    int sockets[NUM_PAIRS][2];
    
    for (int i = 0; i < NUM_PAIRS; ++i) {
        ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[i]));
    }
    
    struct Context {
        int read_fd;
        int write_fd;
        int id;
        bool success;
    };
    
    Context contexts[NUM_PAIRS];
    
    auto worker_func = [](void* arg) -> void* {
        Context* ctx = (Context*)arg;
        
        // Write data
        char write_buf[64];
        snprintf(write_buf, sizeof(write_buf), "Message %d", ctx->id);
        ssize_t n = write(ctx->write_fd, write_buf, strlen(write_buf));
        EXPECT_GT(n, 0);
        
        // Wait and read data
        int ret = bthread_fd_wait(ctx->read_fd, POLLIN);
        EXPECT_EQ(0, ret);
        
        char read_buf[64];
        n = read(ctx->read_fd, read_buf, sizeof(read_buf) - 1);
        EXPECT_GT(n, 0);
        read_buf[n] = '\0';
        
        EXPECT_STREQ(write_buf, read_buf);
        ctx->success = true;
        
        return NULL;
    };
    
    std::vector<bthread_t> threads;
    
    for (int i = 0; i < NUM_PAIRS; ++i) {
        contexts[i].read_fd = sockets[i][1];
        contexts[i].write_fd = sockets[i][0];
        contexts[i].id = i;
        contexts[i].success = false;
        
        bthread_t tid;
        ASSERT_EQ(0, bthread_start_urgent(&tid, NULL, worker_func, &contexts[i]));
        threads.push_back(tid);
    }
    
    for (auto tid : threads) {
        ASSERT_EQ(0, bthread_join(tid, NULL));
    }
    
    for (int i = 0; i < NUM_PAIRS; ++i) {
        ASSERT_TRUE(contexts[i].success) << "Operation " << i << " failed";
        close(sockets[i][0]);
        close(sockets[i][1]);
    }
}

// Test fd_wait timeout with io_uring
TEST_P(IOUringIntegrationTest, fd_wait_timeout) {
    bool use_iouring = GetParam();
    
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    timespec ts = butil::milliseconds_from_now(50);
    
    butil::Timer timer;
    timer.start();
    
    int ret = bthread_fd_timedwait(fds[0], POLLIN, &ts);
    
    timer.stop();
    
    ASSERT_EQ(-1, ret);
    ASSERT_EQ(ETIMEDOUT, errno);
    ASSERT_GE(timer.m_elapsed(), 40);
    ASSERT_LE(timer.m_elapsed(), 100);
    
    close(fds[0]);
    close(fds[1]);
}

// Test closing fd wakes up waiters
TEST_P(IOUringIntegrationTest, close_wakes_waiters) {
    bool use_iouring = GetParam();
    
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    
    auto wait_func = [](void* arg) -> void* {
        int fd = *(int*)arg;
        timespec ts = butil::milliseconds_from_now(5000);
        bthread_fd_timedwait(fd, POLLIN, &ts);
        return NULL;
    };
    
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_urgent(&tid, NULL, wait_func, &fds[0]));
    
    bthread_usleep(10000); // 10ms
    
    butil::Timer timer;
    timer.start();
    
    close(fds[0]);
    
    ASSERT_EQ(0, bthread_join(tid, NULL));
    
    timer.stop();
    
    // Should complete quickly, not wait for full timeout
    ASSERT_LT(timer.m_elapsed(), 1000);
    
    close(fds[1]);
}

// Test brpc Socket with io_uring
TEST_P(IOUringIntegrationTest, brpc_socket_creation) {
    bool use_iouring = GetParam();
    
    brpc::SocketOptions options;
    options.fd = -1;
    
    brpc::SocketId id;
    ASSERT_EQ(0, brpc::Socket::Create(options, &id));
    
    brpc::SocketUniquePtr ptr;
    ASSERT_EQ(0, brpc::Socket::Address(id, &ptr));
    ASSERT_TRUE(ptr.get() != NULL);
    
    // Socket should work with both epoll and io_uring
    LOG(INFO) << "Socket created successfully with " 
              << (use_iouring ? "io_uring" : "epoll");
}

// Test stress scenario with many operations
TEST_P(IOUringIntegrationTest, stress_many_fd_operations) {
    bool use_iouring = GetParam();
    
    const int NUM_OPS = 50; // Reduced from 100 for faster test
    int pipes[NUM_OPS][2];
    
    for (int i = 0; i < NUM_OPS; ++i) {
        ASSERT_EQ(0, pipe(pipes[i]));
    }
    
    struct OpContext {
        int read_fd;
        int write_fd;
        int id;
        bool completed;
    };
    
    OpContext contexts[NUM_OPS];
    
    auto op_func = [](void* arg) -> void* {
        OpContext* ctx = (OpContext*)arg;
        
        // Wait for read event
        int ret = bthread_fd_wait(ctx->read_fd, POLLIN);
        EXPECT_EQ(0, ret);
        
        // Read data
        char buf;
        ssize_t n = read(ctx->read_fd, &buf, 1);
        EXPECT_EQ(1, n);
        EXPECT_EQ('A' + (ctx->id % 26), buf);
        
        ctx->completed = true;
        return NULL;
    };
    
    bthread_t threads[NUM_OPS];
    
    // Start all operations
    for (int i = 0; i < NUM_OPS; ++i) {
        contexts[i].read_fd = pipes[i][0];
        contexts[i].write_fd = pipes[i][1];
        contexts[i].id = i;
        contexts[i].completed = false;
        ASSERT_EQ(0, bthread_start_urgent(&threads[i], NULL, op_func, &contexts[i]));
    }
    
    bthread_usleep(20000); // 20ms
    
    // Write to all pipes
    for (int i = 0; i < NUM_OPS; ++i) {
        char data = 'A' + (i % 26);
        ASSERT_EQ(1, write(pipes[i][1], &data, 1));
    }
    
    // Wait for all operations
    for (int i = 0; i < NUM_OPS; ++i) {
        ASSERT_EQ(0, bthread_join(threads[i], NULL));
        ASSERT_TRUE(contexts[i].completed);
    }
    
    // Cleanup
    for (int i = 0; i < NUM_OPS; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

// Test POLLIN and POLLOUT events
TEST_P(IOUringIntegrationTest, pollin_pollout_events) {
    bool use_iouring = GetParam();
    
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    // Test POLLOUT (should be ready immediately)
    butil::Timer timer;
    timer.start();
    
    int ret = bthread_fd_wait(fds[0], POLLOUT);
    
    timer.stop();
    
    ASSERT_EQ(0, ret);
    ASSERT_LT(timer.m_elapsed(), 50); // Should be immediate
    
    // Test POLLIN with data
    char data = 'X';
    ASSERT_EQ(1, write(fds[1], &data, 1));
    
    ret = bthread_fd_wait(fds[0], POLLIN);
    ASSERT_EQ(0, ret);
    
    char buf;
    ASSERT_EQ(1, read(fds[0], &buf, 1));
    ASSERT_EQ('X', buf);
    
    close(fds[0]);
    close(fds[1]);
}

// Instantiate tests for both epoll and io_uring modes
INSTANTIATE_TEST_SUITE_P(
    EpollAndIOUring,
    IOUringIntegrationTest,
    ::testing::Values(false, true), // false = epoll, true = io_uring
    [](const ::testing::TestParamInfo<IOUringIntegrationTest::ParamType>& info) {
        return info.param ? "IOUring" : "Epoll";
    }
);

} // namespace

#else // !BRPC_ENABLE_IO_URING

// Placeholder test when io_uring is not enabled
TEST(IOUringIntegrationTest, not_enabled) {
    GTEST_SKIP() << "io_uring support not enabled (compile with -DWITH_IO_URING=ON)";
}

#endif // BRPC_ENABLE_IO_URING


