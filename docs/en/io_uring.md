# brpc io_uring Support

## Overview

Starting from this version, brpc supports using io_uring as an event-driven backend, replacing traditional epoll. io_uring is a new asynchronous I/O interface introduced in Linux kernel 5.1, stabilized in version 5.10, offering higher performance and lower latency compared to epoll.

## Advantages

- **Lower system call overhead**: io_uring reduces the number of system calls through shared memory ring buffers
- **Better batching capability**: Can submit multiple I/O operations at once
- **Lower latency**: Reduces context switches between kernel and user space

## System Requirements

- Linux kernel version >= 5.10
- liburing development library (recommended version >= 2.0)
- io_uring support must be enabled at compile time

### Installing liburing

**Ubuntu/Debian:**
```bash
sudo apt-get install liburing-dev
```

**CentOS/RHEL 8+:**
```bash
sudo dnf install liburing-devel
```

**Build from source:**
```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make
sudo make install
```

## Build Configuration

### Building with CMake

```bash
mkdir build && cd build
cmake .. -DWITH_IO_URING=ON
make
```

### Building with Bazel

Add to .bazelrc file:

```
build --define=brpc_enable_io_uring=true
```

Or specify on command line:

```bash
bazel build //... --define=brpc_enable_io_uring=true
```

## Runtime Configuration

By default, even if io_uring support is enabled at compile time, the program will still use epoll. You need to enable it via gflag:

```bash
# Enable io_uring
./your_server --use_iouring=true
```

Or set in code:

```cpp
#include <gflags/gflags.h>

DECLARE_bool(use_iouring);

int main(int argc, char* argv[]) {
    FLAGS_use_iouring = true;
    // ... other initialization code
}
```

## Checking io_uring Availability

At startup, brpc automatically detects if the current kernel supports io_uring. If not supported, even with `--use_iouring=true`, it will automatically fall back to epoll.

You can check if io_uring is successfully enabled through logs:

```
I0000 00:00:00.000000  1234 event_dispatcher.cpp:55] Using io_uring for event dispatching
```

If you see the following warning, it means the current kernel doesn't support io_uring:

```
W0000 00:00:00.000000  1234 event_dispatcher_iouring.cpp:222] io_uring not available, please check kernel version (need >= 5.10)
```

## Performance Optimization Tips

1. **Kernel version**: Recommend using Linux 5.19 or higher, which includes more io_uring performance optimizations
2. **Queue depth**: Currently uses a default queue of 256 entries, may need adjustment for high-concurrency scenarios
3. **CPU affinity**: Works better when combined with bthread CPU affinity settings

## Known Limitations

1. Current implementation doesn't support IORING_OP_POLL_MULTI feature (requires kernel 5.13+)
2. File descriptor registration and deregistration performance can be optimized
3. Direct I/O mode (IORING_OP_READ_FIXED) is not supported

## Troubleshooting

### io_uring Initialization Failure

If io_uring initialization fails, possible reasons:

1. Kernel version too old: Check with `uname -r`
2. ulimit restrictions: Check `ulimit -n` and `/proc/sys/fs/file-max`
3. Permission issues: Some environments may require special permissions

### Performance Not as Expected

1. Ensure kernel version is new enough (>= 5.10)
2. Check system load and CPU usage
3. Compare with epoll mode performance to determine if it suits your scenario

## Compatibility

io_uring support is completely optional and won't affect existing functionality when not enabled:

- Can be disabled at compile time
- Can choose between epoll or io_uring at runtime
- API interface remains identical, no application code changes needed

## References

- [io_uring official documentation](https://kernel.dk/io_uring.pdf)
- [Linux kernel io_uring interface](https://kernel.org/doc/html/latest/io_uring.html)
- [brpc IO model documentation](io.md)

