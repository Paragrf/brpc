# brpc io_uring Support Implementation

## Overview

This implementation adds io_uring support to brpc as an alternative to epoll for event dispatching. The implementation uses **liburing**, the official user-space library for io_uring, ensuring maximum compatibility with Linux kernel 5.10+.

## Key Design Decisions

### 1. Using liburing Instead of Raw Syscalls

**Rationale:**
- liburing provides stable, well-tested API abstractions
- Handles kernel version differences and feature detection automatically
- Reduces implementation complexity and potential bugs
- Official library maintained by io_uring author (Jens Axboe)

### 2. Kernel Compatibility

**Target:** Linux kernel 5.10+
- io_uring was introduced in kernel 5.1
- Kernel 5.10 is an LTS release with stable io_uring features
- All required operations (IORING_OP_POLL_ADD, IORING_OP_POLL_REMOVE) are available in 5.10

### 3. Runtime Detection and Fallback

The implementation includes runtime detection in `event_dispatcher_iouring.cpp`:
```cpp
static bool is_io_uring_available()
```
- Creates a test io_uring instance using `io_uring_queue_init()` to verify kernel support
- Falls back to epoll gracefully if io_uring is unavailable
- No need to recompile for different kernel versions
- All detection logic is self-contained in the implementation file

## Files Added/Modified

### New Files:
1. `src/brpc/event_dispatcher_iouring.cpp` - io_uring-based EventDispatcher (includes runtime detection)
2. `docs/cn/io_uring.md` - Chinese documentation
3. `docs/en/io_uring.md` - English documentation
4. `IO_URING_README.md` - This file

### Modified Files:
1. `src/brpc/event_dispatcher.h` - Added `_io_uring_ctx` member
2. `src/brpc/event_dispatcher.cpp` - Added `FLAGS_use_iouring` gflag
3. `src/brpc/event_dispatcher_epoll.cpp` - Added `_io_uring_ctx` initialization
4. `CMakeLists.txt` - Added `WITH_IO_URING` option and liburing detection
5. `src/CMakeLists.txt` - Added liburing linking
6. `BUILD.bazel` - Added io_uring build configuration

## Build Instructions

### Prerequisites

Install liburing development library:

**Ubuntu/Debian:**
```bash
sudo apt-get install liburing-dev
```

**CentOS/RHEL 8+:**
```bash
sudo dnf install liburing-devel
```

**From source:**
```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure --prefix=/usr
make
sudo make install
```

### CMake Build

```bash
mkdir build && cd build
cmake .. -DWITH_IO_URING=ON
make -j$(nproc)
```

### Bazel Build

Add to `.bazelrc`:
```
build --define=brpc_enable_io_uring=true
```

Or use command line:
```bash
bazel build //... --define=brpc_enable_io_uring=true
```

## Runtime Configuration

### Enable io_uring

By default, brpc uses epoll even if compiled with io_uring support. Enable via gflag:

```bash
./your_server --use_iouring=true
```

Or in code:
```cpp
#include <gflags/gflags.h>

DECLARE_bool(use_iouring);

int main(int argc, char* argv[]) {
    FLAGS_use_iouring = true;
    // ... rest of initialization
}
```

### Verify io_uring is Active

Check logs for:
```
I... io_uring EventDispatcher initialized successfully
```

Or if unavailable:
```
W... io_uring not available, please check kernel version (need >= 5.10)
```

## Implementation Details

### Architecture

```
EventDispatcher (interface)
    ↓
event_dispatcher_iouring.cpp (io_uring implementation)
    ↓
liburing API
    ↓
io_uring syscalls (io_uring_setup, io_uring_enter, etc.)
    ↓
Linux kernel io_uring subsystem
```

### Key Operations

1. **Initialization:** `io_uring_queue_init()` creates ring with 256 entries
2. **Add Poll:** `io_uring_prep_poll_add()` registers fd for events
3. **Wait:** `io_uring_wait_cqe()` waits for completions
4. **Remove Poll:** `io_uring_prep_poll_remove()` unregisters fd

### Event Mapping

| Poll Event | Epoll Event |
|-----------|-------------|
| POLLIN    | EPOLLIN     |
| POLLOUT   | EPOLLOUT    |
| POLLERR   | EPOLLERR    |
| POLLHUP   | EPOLLHUP    |

## Testing

### Verify Kernel Support

```bash
# Check kernel version
uname -r  # Should be >= 5.10

# Test io_uring availability
cat > test_iouring.c << 'EOF'
#include <liburing.h>
#include <stdio.h>

int main() {
    struct io_uring ring;
    if (io_uring_queue_init(2, &ring, 0) < 0) {
        printf("io_uring not available\n");
        return 1;
    }
    printf("io_uring available\n");
    io_uring_queue_exit(&ring);
    return 0;
}
EOF

gcc test_iouring.c -luring -o test_iouring
./test_iouring
```

### Run brpc Tests

```bash
# Build with io_uring
cmake .. -DWITH_IO_URING=ON -DBUILD_UNIT_TESTS=ON
make

# Run tests with io_uring enabled
./test/brpc_event_dispatcher_unittest --use_iouring=true
```

## Performance Considerations

### When to Use io_uring

✅ **Good for:**
- High-concurrency scenarios (1000+ connections)
- Low-latency requirements
- Modern kernel (>= 5.19 recommended)
- Applications with many small I/O operations

❌ **May not help:**
- Low connection count (< 100)
- Older kernels (5.10-5.15 have less optimization)
- Applications with large block I/O

### Tuning

Current implementation uses:
- Queue depth: 256 (default)
- Submission mode: Standard (not SQPOLL)
- Polling mode: Standard (not IOPOLL)

These can be adjusted in `event_dispatcher_iouring.cpp` if needed.

## Troubleshooting

### liburing not found during build

```bash
# Ubuntu/Debian
sudo apt-get install liburing-dev

# CentOS/RHEL
sudo dnf install liburing-devel

# Or build from source
git clone https://github.com/axboe/liburing.git
cd liburing
./configure --prefix=/usr
make && sudo make install
sudo ldconfig
```

### io_uring initialization fails at runtime

1. Check kernel version: `uname -r` (need >= 5.10)
2. Check if io_uring is enabled: `cat /boot/config-$(uname -r) | grep CONFIG_IO_URING`
3. Check ulimit: `ulimit -n` (should be > 1024)
4. Check dmesg for kernel messages: `dmesg | grep io_uring`

### Performance regression

If performance is worse with io_uring:
1. Ensure kernel >= 5.19 (many optimizations added)
2. Try disabling at runtime: `--use_iouring=false`
3. Check CPU usage - io_uring may use more CPU for lower latency
4. Profile with `perf` to identify bottlenecks

## Compatibility Matrix

| Kernel Version | io_uring | Recommended |
|---------------|----------|-------------|
| < 5.10        | ❌ No    | Use epoll   |
| 5.10 - 5.15   | ✅ Yes   | Basic support |
| 5.16 - 5.18   | ✅ Yes   | Good |
| >= 5.19       | ✅ Yes   | ⭐ Excellent |

## References

1. [io_uring Documentation](https://kernel.dk/io_uring.pdf)
2. [liburing GitHub](https://github.com/axboe/liburing)
3. [Linux io_uring API](https://kernel.org/doc/html/latest/io_uring.html)
4. [brpc Documentation](https://github.com/apache/brpc)

## License

Same as brpc: Apache License 2.0

