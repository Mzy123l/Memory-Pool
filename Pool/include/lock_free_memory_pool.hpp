#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <memory>

namespace memory_pool
{
    template <size_t block_size>
    class LockFreeMemoryPool
    {
    private:
        
        // 确保块大小至少能放得下一个指针
        static constexpr size_t actual_block_size =
            (block_size >= sizeof(void*)) ? block_size : sizeof(void*);

        // 内存块节点，使用 union 保证空间复用，同时确保最大对齐
        union alignas(std::max_align_t) Node
        {
            Node* next;
            char data[actual_block_size];

            // 默认构造函数
            Node() : next(nullptr) {}
        };

        // 带有版本号的指针（用于防范 ABA 问题）
        // 必须强制对齐到两倍指针大小，这是底层硬件执行双字CAS的对齐要求
        struct alignas(2 * sizeof(void*)) TaggedPointer
        {
            Node* ptr;
            uintptr_t tag;

            // 原子操作要求类型具备 trivially copyable 特性且支持判等
            bool operator==(const TaggedPointer& other) const noexcept
            {
                return ptr == other.ptr && tag == other.tag;
            }
            bool operator!=(const TaggedPointer& other) const noexcept
            {
                return !(*this == other);
            }
        };
      

        // 内存池的空闲链表头节点
        std::atomic<TaggedPointer> head_;

        // 用于管理每次扩容分配的大块内存（Chunk），方便析构时一次性释放给操作系统
        struct alignas(64) ChunkNode  // 添加缓存行对齐，减少假共享
        {
            ChunkNode* next;
            // 节点之后紧跟实际的 blocks 内存
        };
        std::atomic<ChunkNode*> chunk_list_;

    public:
        LockFreeMemoryPool() noexcept : head_({ nullptr, 0 }), chunk_list_(nullptr) {}

        ~LockFreeMemoryPool()
        {
            // 析构时清理所有分配的物理内存块
            ChunkNode* curr = chunk_list_.load(std::memory_order_acquire);
            while (curr)
            {
                ChunkNode* next = curr->next;
                operator delete[](reinterpret_cast<void*>(curr), std::align_val_t{ alignof(ChunkNode) });
                curr = next;
            }
        }

        // 禁用拷贝和赋值
        LockFreeMemoryPool(const LockFreeMemoryPool&) = delete;
        LockFreeMemoryPool& operator=(const LockFreeMemoryPool&) = delete;

        // 允许移动构造和移动赋值
        LockFreeMemoryPool(LockFreeMemoryPool&& other) noexcept
            : head_(other.head_.load(std::memory_order_relaxed))
            , chunk_list_(other.chunk_list_.load(std::memory_order_relaxed))
        {
            other.head_.store({ nullptr, 0 }, std::memory_order_relaxed);
            other.chunk_list_.store(nullptr, std::memory_order_relaxed);
        }

        LockFreeMemoryPool& operator=(LockFreeMemoryPool&& other) noexcept
        {
            if (this != &other)
            {
                // 清理当前内存池
                this->~LockFreeMemoryPool();
                new (this) LockFreeMemoryPool(std::move(other));
            }
            return *this;
        }

        /**
         * @brief 手动扩容，一次性分配 num_blocks 个块
         */
        void expand(size_t num_blocks)
        {
            if (num_blocks == 0) return;

            // 1. 一次性向操作系统申请连续内存（Chunk 头部 + N 个连续 Node）
            size_t alloc_size = sizeof(ChunkNode) + num_blocks * sizeof(Node);
            void* raw_mem = operator new[](alloc_size, std::align_val_t{ alignof(ChunkNode) });

            // 2. 初始化内存
            std::memset(raw_mem, 0, alloc_size);

            ChunkNode* chunk = reinterpret_cast<ChunkNode*>(raw_mem);
            Node* blocks = reinterpret_cast<Node*>(
                reinterpret_cast<char*>(raw_mem) + sizeof(ChunkNode));

            // 3. 将新分配的块串联成链表
            for (size_t i = 0; i < num_blocks - 1; ++i)
            {
                blocks[i].next = &blocks[i + 1];
            }
            blocks[num_blocks - 1].next = nullptr;

            // 4. 将连续的块以无锁方式整体推入空闲链表
            TaggedPointer current_head = head_.load(std::memory_order_acquire);
            TaggedPointer new_head;

            do
            {
                // 将新链表的尾部连接到当前的旧头部
                blocks[num_blocks - 1].next = current_head.ptr;
                new_head.ptr = &blocks[0]; // 链表头指向第一个新块
                new_head.tag = current_head.tag + 1;  // Tag 加 1，避免 ABA 问题
            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

            // 5. 将新分配的 Chunk 头采用无锁方式挂载到 chunk_list_ (仅用于析构释放)
            ChunkNode* old_chunk_head = chunk_list_.load(std::memory_order_relaxed);
            do
            {
                chunk->next = old_chunk_head;
            } while (!chunk_list_.compare_exchange_weak(
                old_chunk_head, chunk,
                std::memory_order_release,
                std::memory_order_relaxed));
        }

        /**
         * @brief 从内存池申请一个块。如果耗尽且没有自动扩容，将返回 nullptr
         */
        void* allocate() noexcept
        {
            TaggedPointer current_head = head_.load(std::memory_order_acquire);
            TaggedPointer new_head;

            do
            {
                if (current_head.ptr == nullptr)
                {
                    return nullptr; // 内存池已空，需外部调用 expand
                }

                new_head.ptr = current_head.ptr->next;
                new_head.tag = current_head.tag + 1;

            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

            // 确保分配的内存是零初始化的
            std::memset(current_head.ptr, 0, sizeof(Node));
            return current_head.ptr;
        }

        /**
         * @brief 将使用完毕的块释放回内存池
         */
        void deallocate(void* ptr) noexcept
        {
            if (!ptr) return;

            // 可选：在释放前将内存清零，提高安全性
            std::memset(ptr, 0, sizeof(Node));

            Node* node = static_cast<Node*>(ptr);
            TaggedPointer current_head = head_.load(std::memory_order_acquire);
            TaggedPointer new_head;
            new_head.ptr = node;

            do
            {
                node->next = current_head.ptr;
                new_head.tag = current_head.tag + 1; // 每次入队递增版本号防 ABA
            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));
        }

        /**
         * @brief 获取当前可用的块数量（近似值，多线程环境下不精确）
         */
        size_t available_blocks() const noexcept
        {
            size_t count = 0;
            TaggedPointer current = head_.load(std::memory_order_acquire);
            Node* node = current.ptr;

            while (node)
            {
                ++count;
                node = node->next;
            }
            return count;
        }
    };


}