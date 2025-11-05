# io_uring API Compatibility Check for Linux 5.10

## Summary

✅ **All APIs used are compatible with Linux 5.10**

---

## Detailed API Analysis

### liburing Functions Used

| Function | First Available | Used in Code | Compatible with 5.10? |
|----------|----------------|--------------|----------------------|
| `io_uring_queue_init()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_queue_exit()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_get_sqe()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_submit()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_wait_cqe()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_cqe_seen()` | liburing 0.1 (kernel 5.1+) | ❌ Not used | ✅ **N/A** |
| `io_uring_cq_advance()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_prep_poll_add()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_prep_poll_remove()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_sqe_set_data()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_cqe_get_data()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |
| `io_uring_for_each_cqe()` | liburing 0.1 (kernel 5.1+) | ✅ Yes | ✅ **YES** |

### Kernel Operations (Opcodes) Used

| Opcode | First Available | Used in Code | Compatible with 5.10? |
|--------|----------------|--------------|----------------------|
| `IORING_OP_POLL_ADD` | Linux 5.1 | ✅ Yes (via prep_poll_add) | ✅ **YES** |
| `IORING_OP_POLL_REMOVE` | Linux 5.1 | ✅ Yes (via prep_poll_remove) | ✅ **YES** |

### Kernel Features Used

| Feature | First Available | Used in Code | Compatible with 5.10? |
|---------|----------------|--------------|----------------------|
| Basic io_uring | Linux 5.1 | ✅ Yes | ✅ **YES** |
| Poll operations | Linux 5.1 | ✅ Yes | ✅ **YES** |
| Batch submission | Linux 5.1 | ✅ Yes | ✅ **YES** |
| Batch CQE processing | Linux 5.1 | ✅ Yes | ✅ **YES** |

### Kernel Features NOT Used (Higher versions)

| Feature | First Available | Used in Code | Reason Not Used |
|---------|----------------|--------------|-----------------|
| `IORING_POLL_ADD_MULTI` (multishot poll) | Linux 5.13 | ❌ No | Not available in 5.10 |
| `IORING_OP_POLL_ADD_LEVEL` (level-triggered) | Linux 5.13 | ❌ No | Not available in 5.10 |
| `IORING_SETUP_SQPOLL` | Linux 5.1 | ❌ No | Optional, not used |
| `IORING_SETUP_IOPOLL` | Linux 5.1 | ❌ No | Not needed for network I/O |
| `io_uring_submit_and_wait()` | liburing 0.1 | ❌ No | Could be used but not critical |

---

## Code Verification

### 1. Queue Initialization (Line 38, 93)

```cpp
int ret = io_uring_queue_init(256, &ctx->ring, 0);
```

- ✅ `io_uring_queue_init()` available since kernel 5.1
- ✅ No special flags used (3rd parameter is 0)
- ✅ Standard queue initialization

**Verdict:** ✅ **Compatible with 5.10**

---

### 2. Poll Add Operations (Line 237, 269, 316, 396, 460)

```cpp
io_uring_prep_poll_add(sqe, fd, poll_mask);
io_uring_sqe_set_data(sqe, (void*)(uintptr_t)event_data_id);
```

- ✅ `io_uring_prep_poll_add()` uses `IORING_OP_POLL_ADD` (available since 5.1)
- ✅ Standard poll events: `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`
- ❌ NOT using `IORING_POLL_ADD_MULTI` (would require 5.13+)
- ✅ One-shot poll behavior (default in 5.10)

**Verdict:** ✅ **Compatible with 5.10**

---

### 3. Poll Remove Operations (Line 287, 353)

```cpp
io_uring_prep_poll_remove(sqe, (void*)(uintptr_t)event_data_id);
```

- ✅ `io_uring_prep_poll_remove()` uses `IORING_OP_POLL_REMOVE` (available since 5.1)
- ✅ Standard removal by user_data

**Verdict:** ✅ **Compatible with 5.10**

---

### 4. Batch Submission (Line 189, 207, 398)

```cpp
int ret = io_uring_submit(&ctx->ring);
```

- ✅ `io_uring_submit()` available since kernel 5.1
- ✅ Batching is done at application level (accumulating SQEs)
- ✅ No kernel-specific batch features required

**Verdict:** ✅ **Compatible with 5.10**

---

### 5. Batch CQE Processing (Line 412-420)

```cpp
io_uring_for_each_cqe(&ctx->ring, head, cqe) {
    cqes[count++] = cqe;
    if (count >= BATCH_SIZE) break;
}

if (count == 0) {
    int ret = io_uring_wait_cqe(&ctx->ring, &cqe);
}
```

- ✅ `io_uring_for_each_cqe()` is a liburing macro, works with 5.1+
- ✅ `io_uring_wait_cqe()` available since kernel 5.1
- ✅ Just iterates through available CQEs (no special kernel features)

**Verdict:** ✅ **Compatible with 5.10**

---

### 6. Mark CQEs as Seen (Line 524)

```cpp
io_uring_cq_advance(&ctx->ring, count);
```

- ✅ `io_uring_cq_advance()` available in liburing since 0.1
- ✅ Just updates the completion queue head pointer
- ✅ More efficient than calling `io_uring_cqe_seen()` multiple times

**Verdict:** ✅ **Compatible with 5.10**

---

## Special Considerations

### 1. One-Shot Poll Behavior

**In our code:**
```cpp
// After handling event, manually re-arm (Line 508-514)
if (fd >= 0 && !(events & EPOLLHUP)) {
    auto mask_it = ctx->poll_mask_map.find(fd);
    if (mask_it != ctx->poll_mask_map.end()) {
        uint32_t poll_mask = mask_it->second;
        rearm_poll(ctx, fd, event_data_id, poll_mask);
    }
}
```

- ✅ This is the **correct approach for 5.10**
- ✅ Poll operations are one-shot in 5.10 (no multishot)
- ✅ We manually re-arm after each event
- ℹ️ Multishot poll (`IORING_POLL_ADD_MULTI`) was added in 5.13

**Verdict:** ✅ **Correct for 5.10, would be improved in 5.13+**

---

### 2. No SQPOLL Mode

**In our code:**
```cpp
int ret = io_uring_queue_init(256, &ctx->ring, 0);  // flags = 0
```

- ℹ️ Not using `IORING_SETUP_SQPOLL` flag
- ℹ️ SQPOLL is available in 5.10 but optional
- ✅ Standard mode is safer and more compatible
- ℹ️ Could enable SQPOLL later as an optimization

**Verdict:** ✅ **Conservative choice, compatible with all 5.1+**

---

### 3. No IOPOLL Mode

**In our code:**
```cpp
int ret = io_uring_queue_init(256, &ctx->ring, 0);  // flags = 0
```

- ℹ️ Not using `IORING_SETUP_IOPOLL` flag
- ✅ IOPOLL is for block devices, not needed for network I/O
- ✅ Correct choice for brpc use case

**Verdict:** ✅ **Correct choice for network I/O**

---

## liburing Version Requirements

### Minimum liburing Version

- **Required:** liburing 0.1 or higher
- **Recommended:** liburing 0.7 or higher
- **All APIs used:** Available in liburing 0.1+

### Check liburing Version

```bash
# Check installed version
pkg-config --modversion liburing

# Or check header
grep "LIBURING_VERSION" /usr/include/liburing.h
```

---

## Kernel Version Verification

### Runtime Check in Code

```cpp
static bool is_io_uring_available() {
    struct io_uring ring;
    int ret = io_uring_queue_init(2, &ring, 0);
    if (ret < 0) {
        return false;  // io_uring not available
    }
    io_uring_queue_exit(&ring);
    return true;
}
```

- ✅ Tests actual kernel support at runtime
- ✅ Gracefully handles unsupported kernels
- ✅ No compile-time kernel version dependency

---

## Compatibility Matrix

| Linux Kernel | io_uring Support | Our Code Compatible? | Notes |
|--------------|------------------|---------------------|--------|
| < 5.1 | ❌ No | ❌ No | io_uring not available |
| 5.1 - 5.9 | ⚠️ Limited | ✅ Yes | Basic features, may have bugs |
| 5.10 - 5.12 | ✅ Stable | ✅ **YES** | **Target version** |
| 5.13+ | ✅ Enhanced | ✅ Yes | Has multishot poll (not used) |
| 5.19+ | ✅ Optimized | ✅ Yes | Performance improvements |

---

## API Usage Summary

### ✅ Compatible APIs (Used in Code)

1. ✅ `io_uring_queue_init()` - Initialize ring (5.1+)
2. ✅ `io_uring_queue_exit()` - Clean up ring (5.1+)
3. ✅ `io_uring_get_sqe()` - Get submission entry (5.1+)
4. ✅ `io_uring_submit()` - Submit operations (5.1+)
5. ✅ `io_uring_wait_cqe()` - Wait for completion (5.1+)
6. ✅ `io_uring_for_each_cqe()` - Iterate completions (5.1+)
7. ✅ `io_uring_cq_advance()` - Mark multiple as seen (5.1+)
8. ✅ `io_uring_prep_poll_add()` - Add poll operation (5.1+)
9. ✅ `io_uring_prep_poll_remove()` - Remove poll operation (5.1+)
10. ✅ `io_uring_sqe_set_data()` - Set user data (5.1+)
11. ✅ `io_uring_cqe_get_data()` - Get user data (5.1+)

### ❌ Incompatible APIs (NOT Used)

1. ❌ `IORING_POLL_ADD_MULTI` - Multishot poll (5.13+, not used)
2. ❌ `IORING_OP_POLL_ADD_LEVEL` - Level-triggered (5.13+, not used)
3. ❌ Any 5.11+ specific features - Not used

---

## Testing on Different Kernel Versions

### Test Plan

```bash
# Test on 5.10
uname -r  # Should be 5.10.x
./brpc_event_dispatcher_iouring_unittest

# Test on 5.4 (should fail gracefully)
# Should see: "io_uring not available, please check kernel version"

# Test on 5.13+ (should work, no multishot though)
# Should work identically to 5.10
```

---

## Conclusion

### ✅ **CONFIRMED: All modifications are compatible with Linux 5.10**

**Key Points:**

1. ✅ All liburing APIs used are from version 0.1 (compatible with kernel 5.1+)
2. ✅ Only using `IORING_OP_POLL_ADD` and `IORING_OP_POLL_REMOVE` (available in 5.1+)
3. ✅ No usage of multishot poll or any 5.11+ features
4. ✅ Manual re-arming is the correct approach for 5.10
5. ✅ Batch processing is done at application level (no kernel version dependency)
6. ✅ Runtime detection ensures graceful fallback on unsupported kernels

**Summary:**

- **Minimum kernel:** Linux 5.1 (basic io_uring)
- **Recommended kernel:** Linux 5.10+ (stable LTS)
- **All code:** Compatible with 5.10 ✅
- **Future enhancement:** Can use multishot poll when min kernel becomes 5.13+

---

## References

1. [Linux kernel 5.10 changelog](https://kernelnewbies.org/Linux_5.10)
2. [io_uring documentation](https://kernel.org/doc/html/v5.10/io_uring.html)
3. [liburing GitHub](https://github.com/axboe/liburing)
4. [io_uring feature matrix](https://kernel.dk/io_uring-whatsnew.html)


