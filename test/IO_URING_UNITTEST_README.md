# io_uring Unit Tests

This document describes the io_uring related unit tests added to brpc.

## Overview

Two new test files have been added to comprehensively test io_uring functionality in brpc:

1. **`brpc_event_dispatcher_iouring_unittest.cpp`** - Core io_uring functionality tests
2. **`bthread_fd_iouring_unittest.cpp`** - bthread integration with io_uring tests

These tests mirror the existing epoll tests but are specifically designed to validate io_uring implementation.

## Test Files

### 1. brpc_event_dispatcher_iouring_unittest.cpp

Tests the low-level io_uring operations and event dispatcher functionality.

**Test Cases:**

- `io_uring_availability` - Verifies io_uring is available on the system
- `queue_init_sizes` - Tests different queue sizes (2, 64, 256 entries)
- `poll_add` - Tests adding poll operations
- `poll_with_event` - Tests poll operations with actual events
- `poll_remove` - Tests removing poll operations
- `multiple_fds` - Tests handling multiple file descriptors simultaneously
- `timeout` - Tests timeout functionality
- `edge_triggered_behavior` - Tests edge-triggered event handling
- `closed_fd` - Tests behavior with closed file descriptors
- `pollin_pollout_events` - Tests POLLIN and POLLOUT event types
- `invalid_fd` - Tests error handling with invalid file descriptors
- `queue_overflow` - Tests queue overflow scenarios
- `rearm_poll` - Tests re-arming poll operations after events

**Key Features Tested:**
- `io_uring_queue_init()` - Queue initialization
- `io_uring_prep_poll_add()` - Adding poll operations
- `io_uring_prep_poll_remove()` - Removing poll operations
- `io_uring_wait_cqe()` - Waiting for completion events
- `io_uring_submit()` - Submitting operations
- Event handling (POLLIN, POLLOUT, POLLERR, POLLHUP)
- Error handling and edge cases

### 2. bthread_fd_iouring_unittest.cpp

Tests the integration of bthread fd operations with io_uring backend.

**Test Cases:**

- `basic_fd_wait` - Basic bthread_fd_wait functionality
- `fd_timedwait_timeout` - Tests timeout in bthread_fd_timedwait
- `fd_timedwait_event` - Tests event arrival before timeout
- `multiple_bthread_wait` - Tests multiple bthreads waiting on different fds
- `fd_wait_pollout` - Tests POLLOUT event waiting
- `close_wakes_waiter` - Tests that closing fd wakes up waiting threads
- `invalid_fd` - Tests error handling with invalid fds
- `invalid_events` - Tests error handling with invalid event masks
- `concurrent_read_write_waiters` - Tests concurrent read/write operations
- `stress_many_operations` - Stress test with 100 concurrent operations
- `cancelled_wait` - Tests cancellation of wait operations
- `sequential_operations` - Tests sequential read/write operations
- `fd_wait_in_pthread` - Tests fd operations in pthread context

**Key Features Tested:**
- `bthread_fd_wait()` - Wait for fd events
- `bthread_fd_timedwait()` - Wait with timeout
- `bthread_close()` - Close fd and wake waiters
- Multiple bthread coordination
- Event type handling (POLLIN, POLLOUT, POLLET)
- Error handling (EINVAL, ETIMEDOUT, etc.)
- Integration with bthread scheduler

## Build Requirements

To build and run these tests, you need:

1. **Linux kernel >= 5.10** with io_uring support
2. **liburing** library installed
3. CMake build flag: `-DWITH_IO_URING=ON`

### Installation

#### Ubuntu/Debian:
```bash
sudo apt-get install liburing-dev
```

#### From source:
```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make
sudo make install
```

## Building Tests

```bash
# Configure with io_uring support
mkdir build && cd build
cmake .. -DWITH_IO_URING=ON -DBUILD_UNIT_TESTS=ON

# Build tests
make -j$(nproc)

# The test binaries will be generated:
# - test/brpc_event_dispatcher_iouring_unittest
# - test/bthread_fd_iouring_unittest
```

## Running Tests

### Run all io_uring tests:
```bash
cd test

# Run event dispatcher tests
./brpc_event_dispatcher_iouring_unittest

# Run bthread fd tests
./bthread_fd_iouring_unittest --use_iouring=true
```

### Run specific test:
```bash
# Run a specific test case
./brpc_event_dispatcher_iouring_unittest --gtest_filter=IOUringEventDispatcherTest.poll_with_event

# Run with verbose output
./bthread_fd_iouring_unittest --use_iouring=true --v=1
```

### Run with CTest:
```bash
# Run all tests including io_uring tests
ctest -R iouring

# Run with verbose output
ctest -V -R iouring
```

## Test Behavior

### When io_uring is NOT available:

If the system doesn't support io_uring (kernel < 5.10 or not enabled), the tests will be **skipped** with a message:

```
[ SKIPPED ] io_uring not available on this system (kernel < 5.10 or not enabled)
```

This ensures tests don't fail on systems without io_uring support.

### When io_uring support is NOT compiled:

If brpc is built without `-DWITH_IO_URING=ON`, the tests will show:

```
[ SKIPPED ] io_uring support not enabled (compile with -DWITH_IO_URING=ON)
```

## Checking io_uring Availability

To check if your system supports io_uring:

```bash
# Check kernel version (need >= 5.10)
uname -r

# Check if io_uring is enabled in kernel
cat /boot/config-$(uname -r) | grep CONFIG_IO_URING

# Try to load io_uring module
sudo modprobe io_uring

# Check if liburing is installed
ldconfig -p | grep liburing
```

## Test Coverage

The tests cover the following scenarios:

### Normal Operations:
- ✅ Basic poll add/remove operations
- ✅ Multiple file descriptors
- ✅ Multiple bthreads
- ✅ Concurrent read/write operations
- ✅ Sequential operations

### Event Types:
- ✅ POLLIN (readable)
- ✅ POLLOUT (writable)
- ✅ POLLET (edge-triggered)
- ✅ POLLERR (error)
- ✅ POLLHUP (hang up)

### Error Handling:
- ✅ Invalid file descriptors
- ✅ Invalid event masks
- ✅ Closed file descriptors
- ✅ Queue overflow
- ✅ Timeout scenarios

### Integration:
- ✅ bthread_fd_wait
- ✅ bthread_fd_timedwait
- ✅ bthread_close
- ✅ pthread compatibility

### Stress Testing:
- ✅ 100+ concurrent operations
- ✅ Rapid add/remove cycles
- ✅ Multiple event types simultaneously

## Comparing with epoll Tests

The io_uring tests are designed to mirror the existing epoll tests:

| epoll Test | io_uring Test | Purpose |
|------------|---------------|---------|
| `test/brpc_event_dispatcher_unittest.cpp` | `test/brpc_event_dispatcher_iouring_unittest.cpp` | Event dispatcher tests |
| `test/bthread_fd_unittest.cpp` (epoll parts) | `test/bthread_fd_iouring_unittest.cpp` | bthread fd integration |

## Debugging Failed Tests

If tests fail, try:

```bash
# Enable verbose logging
export GLOG_v=3
./brpc_event_dispatcher_iouring_unittest --v=3

# Check kernel messages
sudo dmesg | grep io_uring

# Verify io_uring works at system level
./brpc_event_dispatcher_iouring_unittest --gtest_filter=*availability*
```

## Performance Considerations

io_uring tests may have different performance characteristics than epoll tests:

- **Lower latency** for high-throughput scenarios
- **Lower CPU overhead** for many concurrent operations
- **Higher memory usage** due to ring buffers

The tests include stress scenarios to validate performance under load.

## Contributing

When adding new io_uring tests:

1. Mirror existing epoll test structure
2. Use `GTEST_SKIP()` when io_uring is unavailable
3. Test both success and error paths
4. Include stress/concurrency tests
5. Document test purpose clearly
6. Follow the existing naming convention

## References

- [Linux io_uring Documentation](https://kernel.org/doc/html/latest/io_uring.html)
- [liburing Library](https://github.com/axboe/liburing)
- [brpc io_uring Implementation](../IO_URING_README.md)
- [Original epoll tests](bthread_fd_unittest.cpp)

## Troubleshooting

### Test compilation fails:

```bash
# Make sure liburing is installed
sudo apt-get install liburing-dev

# Verify CMake found liburing
cmake .. -DWITH_IO_URING=ON
# Look for: "io_uring support enabled with liburing at ..."
```

### Tests are skipped:

```bash
# Check kernel version
uname -r  # Should be >= 5.10

# Verify io_uring support
cat /boot/config-$(uname -r) | grep CONFIG_IO_URING
# Should show: CONFIG_IO_URING=y
```

### Tests fail with "Operation not permitted":

```bash
# May need elevated privileges or container capabilities
sudo ./brpc_event_dispatcher_iouring_unittest

# Or add CAP_SYS_ADMIN capability
sudo setcap cap_sys_admin+ep ./brpc_event_dispatcher_iouring_unittest
```

## License

Same as brpc - Apache License 2.0


