# C++20 无锁内存池 (Lock-Free Memory Pool)

@WARNING 实现需要双字无锁支持，必须保证平台支持双字无锁，并设置对应的编译选项，否则将回退至有锁实现，无法保证性能与可用性！！！

一个基于 C++20 标准的无锁内存池实现，专为高频、多线程的小块内存分配与释放场景设计，旨在提供比系统默认 `malloc/free` 更高的性能和更低的开销。


## ✨ 核心特性

一个基于 C++20 标准的无锁内存池实现，专为高频、多线程的小块内存分配与释放场景设计，旨在提供比系统默认 `malloc/free new/delete` 更高的性能和更低的开销。

- **无锁设计 (需平台支持)**：基于原子操作实现，避免了线程阻塞，极大提升了高并发场景下的吞吐量。
- **C++20 标准**：充分利用现代 C++ 特性，保证代码的先进性与安全性。
- **固定块大小**：针对特定尺寸的对象进行优化，有效减少内存碎片。
- **手动扩容**：用户可根据实际需求灵活控制内存的预分配时机。

## 📦 API 概览

### `LockFreeMemoryPool<BlockSize>`
类模板，管理指定大小的内存块。

#### 公共方法

| 方法 | 描述 | 时间复杂度 |
| :--- | :--- | :--- |
| `LockFreeMemoryPool() noexcept` | 默认构造函数，创建一个初始为空的内存池。 | O(1) |
| `LockFreeMemoryPool(LockFreeMemoryPool&& other) noexcept` | 移动构造函数。 | O(1) |
| ` LockFreeMemoryPool& operator=(LockFreeMemoryPool&& other) noexcept` | 移动赋值运算符。 | O(1) |
| `void expand(size_t num_blocks)` | **扩容**。向系统申请内存并切分为 `num_blocks` 个块加入空闲链表。 | O(num_blocks) |
| `void* allocate() noexcept` | **申请内存**。从空闲链表头部获取一个块。**注意：如果内存池耗尽，本实现不自动扩容，而是返回 `nullptr`。** | 平均 O(1) |
| `void deallocate(void* ptr) noexcept` | **释放内存**。将指针归还给空闲链表。传入 `nullptr` 是安全的。 | 平均 O(1) |
| `size_t available_blocks() const noexcept` | **查询近似可用数**。返回当前空闲块的大致数量。**警告**：由于无锁读取，此值在并发环境下是瞬时的、不精确的。 | O(N) |

## 🚀 快速开始

### 1. 环境要求
- **C++ 编译器**：支持 C++20 标准（推荐 GCC 10+，Clang 10+，MSVC 19.28+）。
- **构建工具**：CMake 3.10+（可选）。

### 2. 代码示例

cpp
````
#include <iostream>
#include <vector>
#include <thread>
#include "lock_free_memory_pool.hpp" // 假设头文件名为此


#include "lock_free_memory_pool.hpp" // 假设头文件名为此, 根据实际路径调整

int main()
{

    // 1. 创建一个管理 64 字节块的内存池
    memory_pool::LockFreeMemoryPool<64> pool;

    // 2. 预分配 1024 个内存块
    pool.expand(1024);

    // 3. 模拟多线程分配与释放
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) 
    {
        threads.emplace_back([&pool]() 
        {
            for (int j = 0; j < 10000; ++j) 
            {
                void* block = pool.allocate();
                if (block) 
                {
                    // 使用内存...
                    // ...
                    
                    // 释放内存
                    pool.deallocate(block);
                }
            }
        });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    std::cout << "可用块数: " << pool.available_blocks() << std::endl;
    return 0;
}

````
## ⚙️ 构建指南

### 作为 Header-Only 库使用
直接将 `lock_free_memory_pool.hpp` 复制到您的项目中，并在需要使用的源文件中包含它即可：
`````
#include "path/to/lock_free_memory_pool.hpp"
`````

### 使用 CMake 集成
在你的 `CMakeLists.txt` 中，确保设置了 C++20 标准：

cmake
=======
````

cmake_minimum_required(VERSION 3.10)
project(MyProject)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(my_app src/main.cpp)



````


## 📝 设计与注意事项

- **固定块大小**：该内存池仅管理固定大小的内存块。如果需要分配不同大小的对象，请为每种大小实例化一个独立的内存池。
- **手动扩容策略**：为了极致性能，本实现**不提供**自动扩容机制。当 `allocate()` 返回 `nullptr` 时，意味着当前池已耗尽，需要调用者主动调用 `expand()` 来增加容量。
- **ABA 问题防护**：底层实现通过双字原子操作（Double-Word CAS）来防范无锁编程中的 ABA 问题，保证链表指针操作的原子性与安全性。
- **双字无锁问题**： 需要注意的是，某些平台可能不支持双字原子操作，在这种情况下，性能可能会受到影响，甚至无法正确工作。

## 📄 许可证

本项目采用 **MIT License** 开源协议。详情请参阅项目根目录下的 `LICENSE` 文件。

