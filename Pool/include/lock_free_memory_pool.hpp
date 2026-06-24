#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <memory>
#include <cassert>
#include <thread>

namespace memory_pool
{
    template <size_t block_size>
    class LockFreeMemoryPool
    {
    private:
        static constexpr size_t actual_block_size =
            (block_size >= sizeof(void*)) ? block_size : sizeof(void*);

        // 金丝雀哨兵，精准拦截外部 Double Free 和 内存越界越位写
        static constexpr uint32_t ALLOCATED_MAGIC = 0xDEADBEEF;
        static constexpr uint32_t FREED_MAGIC = 0x0D15EA5E;

        // 内存块节点
        struct alignas(std::max_align_t) Node
        {
            char data[actual_block_size]; // 放在首位，确保 void* ptr 能完美强转回 Node*
            uint32_t canary;              // 哨兵变量
            std::atomic<Node*> next;      // 严格原子指针

            // 构造时标记为“已在空闲池中”
            Node() noexcept : canary(FREED_MAGIC), next(nullptr) {}
        };

        // 带有版本号的指针（防范 ABA）
        struct alignas(2 * sizeof(void*)) TaggedPointer
        {
            Node* ptr;
            uintptr_t tag;

            bool operator==(const TaggedPointer& other) const noexcept
            {
                return ptr == other.ptr && tag == other.tag;
            }
            bool operator!=(const TaggedPointer& other) const noexcept
            {
                return !(*this == other);
            }
        };

        // 用于管理物理大内存块（Chunk）的节点
        struct alignas(64) ChunkNode
        {
            ChunkNode* next;
        };

        // 强制 64 字节对齐，防止 head_ 和 chunk_list_ 发生伪共享
        alignas(64) std::atomic<TaggedPointer> head_;
        alignas(64) std::atomic<ChunkNode*> chunk_list_;

    public:
        LockFreeMemoryPool() noexcept : head_({ nullptr, 0 }), chunk_list_(nullptr) {}

        ~LockFreeMemoryPool()
        {
            clear();
        }

        LockFreeMemoryPool(const LockFreeMemoryPool&) = delete;
        LockFreeMemoryPool& operator=(const LockFreeMemoryPool&) = delete;

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
                clear();
                head_.store(other.head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                chunk_list_.store(other.chunk_list_.load(std::memory_order_relaxed), std::memory_order_relaxed);

                other.head_.store({ nullptr, 0 }, std::memory_order_relaxed);
                other.chunk_list_.store(nullptr, std::memory_order_relaxed);
            }
            return *this;
        }

        /**
         * @brief 从内存池申请一个块
         */
        void* allocate() noexcept
        {
            // 初始加载使用 relaxed，减少屏障开销
            TaggedPointer current_head = head_.load(std::memory_order_relaxed);
            TaggedPointer new_head;
            int spin_count = 0;

            do
            {
                if (current_head.ptr == nullptr)
                {
                    return nullptr; // 内存池已空
                }

                // acquire 保证读取 next 时数据的可见性
                new_head.ptr = current_head.ptr->next.load(std::memory_order_acquire);
                new_head.tag = current_head.tag + 1;

                // 简单的退避策略，缓解高并发下 CAS 频繁失败导致的缓存颠簸
                if (spin_count++ > 8)
                {
                    std::this_thread::yield();
                }

            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

            assert(current_head.ptr->canary == FREED_MAGIC &&
                "Memory Pool Crash: Internal list corrupted by buffer overflow!");

            current_head.ptr->canary = ALLOCATED_MAGIC;

          
            return current_head.ptr->data;
        }

        /**
         * @brief 将使用完毕的块释放回内存池
         */
        void deallocate(void* ptr) noexcept
        {
            if (!ptr) return;

            Node* node = reinterpret_cast<Node*>(ptr);

            assert(node->canary == ALLOCATED_MAGIC &&
                "Memory Pool Crash: Double Free or Buffer Overflow detected in user code!");

            node->canary = FREED_MAGIC;


            TaggedPointer current_head = head_.load(std::memory_order_relaxed);
            TaggedPointer new_head;
            new_head.ptr = node;
            int spin_count = 0;

            do
            {
                node->next.store(current_head.ptr, std::memory_order_relaxed);
                new_head.tag = current_head.tag + 1;

                if (spin_count++ > 8)
                {
                    std::this_thread::yield();
                }

            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));
        }

        /**
         * @brief 手动扩容
         */
        void expand(size_t num_blocks = 32)
        {
            if (num_blocks == 0) return;

            size_t alloc_size = sizeof(ChunkNode) + num_blocks * sizeof(Node);
            void* raw_mem = operator new[](alloc_size, std::align_val_t{ alignof(ChunkNode) });

            ChunkNode* chunk = new (raw_mem) ChunkNode{ nullptr };
            Node* blocks = reinterpret_cast<Node*>(
                reinterpret_cast<char*>(raw_mem) + sizeof(ChunkNode));

            ChunkNode* old_chunk_list = chunk_list_.load(std::memory_order_relaxed);
            do
            {
                chunk->next = old_chunk_list;
            } while (!chunk_list_.compare_exchange_weak(
                old_chunk_list, chunk,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

            for (size_t i = 0; i < num_blocks; ++i)
            {
                new (&blocks[i]) Node();
            }

            for (size_t i = 0; i < num_blocks - 1; ++i)
            {
                blocks[i].next.store(&blocks[i + 1], std::memory_order_relaxed);
            }
            blocks[num_blocks - 1].next.store(nullptr, std::memory_order_relaxed);

            TaggedPointer current_head = head_.load(std::memory_order_relaxed);
            TaggedPointer new_head;
            do
            {
                blocks[num_blocks - 1].next.store(current_head.ptr, std::memory_order_relaxed);
                new_head.ptr = &blocks[0];
                new_head.tag = current_head.tag + 1;
            } while (!head_.compare_exchange_weak(
                current_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire));
        }

        void clear() noexcept
        {
            ChunkNode* curr = chunk_list_.exchange(nullptr, std::memory_order_acq_rel);
            while (curr)
            {
                ChunkNode* next = curr->next;
                operator delete[](reinterpret_cast<void*>(curr), std::align_val_t{ alignof(ChunkNode) });
                curr = next;
            }
            head_.store({ nullptr, 0 }, std::memory_order_relaxed);
        }

        size_t available_blocks() const noexcept
        {
            size_t count = 0;
            TaggedPointer current = head_.load(std::memory_order_acquire);
            Node* curr_node = current.ptr;
            while (curr_node)
            {
                count++;
                curr_node = curr_node->next.load(std::memory_order_acquire);
            }
            return count;
        }
    };
}