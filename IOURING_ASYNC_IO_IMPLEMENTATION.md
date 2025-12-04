# io_uring 异步 I/O 实现说明

## 概述

本实现为 brpc 添加了基于 io_uring 的异步 I/O 支持，允许在关键路径使用真正的异步 I/O 操作（`io_uring_prep_readv`/`io_uring_prep_writev`），而不仅仅是事件通知。

## 实现状态

### ✅ 已实现

1. **异步 I/O 框架**
   - `AsyncIORequest` 结构体：管理异步 I/O 请求
   - 请求生命周期管理：自动清理完成的请求
   - 完成回调机制：支持用户自定义完成回调

2. **提交接口**
   - `submit_async_read()`: 提交异步读操作
   - `submit_async_write()`: 提交异步写操作
   - 公共 C API：`brpc_io_uring_submit_async_read/write()`

3. **完成处理**
   - 在 `EventDispatcher::Run()` 中处理异步 I/O 完成事件
   - 使用高位标志区分异步 I/O 和事件通知
   - 自动调用完成回调

4. **配置选项**
   - `--use_iouring_async_io`: 启用异步 I/O
   - `--iouring_async_io_threshold`: 异步 I/O 阈值（默认 64KB）

### ✅ 已实现

1. **IOBuf 集成** ✅
   - ✅ 在 `IOPortal::pappend_from_file_descriptor()` 中添加异步路径
   - ✅ 在 `IOBuf::pcut_into_file_descriptor()` 中添加异步路径
   - ✅ 根据数据大小自动选择同步/异步（阈值控制）

2. **Socket 层集成** ✅
   - ✅ Socket 层通过 IOBuf 自动使用异步 I/O（大块数据）
   - ✅ 小块数据仍使用同步方式（保持兼容性）

## 使用方法

### 1. 启用异步 I/O

```bash
# 启用 io_uring 和异步 I/O
./your_server --use_iouring=true --use_iouring_async_io=true

# 自定义阈值（字节）
./your_server --use_iouring=true --use_iouring_async_io=true --iouring_async_io_threshold=131072
```

### 2. 使用 C API

```cpp
#include <sys/uio.h>

// 定义完成回调
void read_complete(void* user_data, ssize_t result, int error) {
    if (error) {
        // 处理错误
        return;
    }
    // 处理读取的数据（数据已在 iovecs 中）
    // ...
}

// 提交异步读操作
struct iovec iovecs[2];
iovecs[0].iov_base = buffer1;
iovecs[0].iov_len = 4096;
iovecs[1].iov_base = buffer2;
iovecs[1].iov_len = 4096;

int ret = brpc_io_uring_submit_async_read(fd, iovecs, 2, my_data, read_complete);
if (ret < 0) {
    // 异步 I/O 不可用，使用同步方式
    ssize_t n = readv(fd, iovecs, 2);
}
```

## 实现细节

### 请求标识

使用 64 位请求 ID，高位设置为 1 以区分异步 I/O 请求和事件数据 ID：

```cpp
uint64_t request_id = ctx->next_async_request_id++;
io_uring_sqe_set_data(sqe, (void*)(uintptr_t)(request_id | 0x8000000000000000ULL));
```

### 完成处理

在 `EventDispatcher::Run()` 中：

```cpp
uint64_t user_data_val = (uint64_t)(uintptr_t)user_data;
if (user_data_val & 0x8000000000000000ULL) {
    // 这是异步 I/O 完成
    uint64_t request_id = user_data_val & 0x7FFFFFFFFFFFFFFFULL;
    // 查找请求并调用回调
}
```

### 内存管理

- `AsyncIORequest` 负责管理请求生命周期
- `iovecs` 由调用者拥有（`owns_iovecs = false`）
- 完成时自动删除请求对象

## 实现细节

### 1. IOBuf 集成 ✅

已在 `src/butil/iobuf.cpp` 中实现：

- **读取路径** (`IOPortal::pappend_from_file_descriptor`):
  - 检查是否启用异步 I/O 且数据大小超过阈值
  - 使用 `butex` 实现同步等待
  - 自动降级到同步 I/O（如果异步失败）

- **写入路径** (`IOBuf::pcut_into_file_descriptor`):
  - 检查是否启用异步 I/O 且数据大小超过阈值
  - 在完成回调中自动调用 `pop_front()`
  - 自动降级到同步 I/O（如果异步失败）

### 2. Socket 层集成 ✅

Socket 层通过 IOBuf 自动使用异步 I/O：
- `Socket::DoRead()` 调用 `_read_buf.append_from_file_descriptor()`
- `Socket::StartWrite()` 调用 `req->data.cut_into_file_descriptor()`
- 当数据大小超过阈值时，自动使用异步 I/O

## 性能考虑

### 适用场景

- ✅ **大块数据传输**（>64KB）：显著性能提升
- ✅ **高并发场景**：减少系统调用
- ✅ **低延迟要求**：异步处理，不阻塞

### 不适用场景

- ❌ **小块数据**（<64KB）：同步 I/O 更高效
- ❌ **频繁的小操作**：异步开销可能超过收益

### 阈值选择

- **默认 64KB**：适合大多数场景
- **高吞吐量场景**：可以降低到 32KB
- **低延迟场景**：可以提高到 128KB

## 注意事项

1. **线程安全**：异步 I/O 回调在 EventDispatcher 线程中执行
2. **内存管理**：确保 iovecs 在回调执行期间有效
3. **错误处理**：回调中需要处理错误情况
4. **兼容性**：如果异步 I/O 不可用，自动降级到同步方式

## 测试建议

1. **功能测试**：验证异步 I/O 正常工作
2. **性能测试**：对比同步/异步 I/O 性能
3. **压力测试**：高并发场景下的稳定性
4. **降级测试**：异步 I/O 不可用时的行为

## 参考资料

- [io_uring 官方文档](https://kernel.dk/io_uring.pdf)
- [Linux 内核 io_uring 接口](https://kernel.org/doc/html/latest/io_uring.html)
- [brpc io_uring 支持文档](docs/cn/io_uring.md)

