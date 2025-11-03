# brpc io_uring 支持

## 简介

从本版本开始，brpc支持使用io_uring作为事件驱动的后端，替代传统的epoll。io_uring是Linux内核5.1引入的新异步I/O接口，在5.10版本趋于稳定，相比epoll提供了更高的性能和更低的延迟。

## 优势

- **更低的系统调用开销**: io_uring通过共享内存环形缓冲区减少了系统调用次数
- **更好的批处理能力**: 可以一次性提交多个I/O操作
- **更低的延迟**: 减少了内核态和用户态之间的切换

## 系统要求

- Linux内核版本 >= 5.10
- liburing开发库 (推荐版本 >= 2.0)
- 编译时需要启用io_uring支持

### 安装liburing

**Ubuntu/Debian:**
```bash
sudo apt-get install liburing-dev
```

**CentOS/RHEL 8+:**
```bash
sudo dnf install liburing-devel
```

**从源码安装:**
```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make
sudo make install
```

## 编译配置

### 使用CMake编译

```bash
mkdir build && cd build
cmake .. -DWITH_IO_URING=ON
make
```

### 使用Bazel编译

在.bazelrc文件中添加：

```
build --define=brpc_enable_io_uring=true
```

或者在命令行中指定：

```bash
bazel build //... --define=brpc_enable_io_uring=true
```

## 运行时配置

默认情况下，即使编译时启用了io_uring支持，程序仍会使用epoll。需要通过gflag来启用：

```bash
# 启用io_uring
./your_server --use_iouring=true
```

或者在代码中设置：

```cpp
#include <gflags/gflags.h>

DECLARE_bool(use_iouring);

int main(int argc, char* argv[]) {
    FLAGS_use_iouring = true;
    // ... 其他初始化代码
}
```

## 检查io_uring是否可用

程序启动时，brpc会自动检测当前内核是否支持io_uring。如果不支持，即使设置了`--use_iouring=true`，也会自动降级到epoll。

你可以通过日志查看是否成功启用io_uring：

```
I0000 00:00:00.000000  1234 event_dispatcher.cpp:55] Using io_uring for event dispatching
```

如果看到以下警告，说明当前内核不支持io_uring：

```
W0000 00:00:00.000000  1234 event_dispatcher_iouring.cpp:222] io_uring not available, please check kernel version (need >= 5.10)
```

## 性能优化建议

1. **内核版本**: 建议使用Linux 5.19或更高版本，包含了更多io_uring性能优化
2. **队列深度**: 当前默认使用256个条目的队列，对于高并发场景可能需要调整
3. **CPU亲和性**: 配合bthread的CPU亲和性设置使用效果更好

## 已知限制

1. 当前实现不支持IORING_OP_POLL_MULTI特性（需要内核5.13+）
2. 文件描述符注册和取消注册的性能还有优化空间
3. 不支持直接I/O模式（IORING_OP_READ_FIXED）

## 故障排查

### io_uring初始化失败

如果遇到io_uring初始化失败，可能的原因：

1. 内核版本不够: 使用`uname -r`检查内核版本
2. ulimit限制: 检查`ulimit -n`和`/proc/sys/fs/file-max`
3. 权限问题: 某些环境可能需要特殊权限

### 性能不如预期

1. 确认内核版本足够新（>= 5.10）
2. 检查系统负载和CPU使用率
3. 对比epoll模式的性能，判断是否适合你的场景

## 兼容性

io_uring支持是完全可选的，不启用时不会影响现有功能：

- 编译时可以不启用io_uring支持
- 运行时可以选择使用epoll或io_uring
- API接口完全一致，无需修改应用代码

## 参考资料

- [io_uring官方文档](https://kernel.dk/io_uring.pdf)
- [Linux内核io_uring接口](https://kernel.org/doc/html/latest/io_uring.html)
- [brpc IO模型文档](io.md)

