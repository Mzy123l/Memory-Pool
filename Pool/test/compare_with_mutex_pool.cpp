#include "../include/lock_free_memory_pool.hpp"

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>
#include <atomic>
#include <iomanip>

// =============== 基于锁的内存池（简单实现） ===============
template <size_t block_size>
class MutexMemoryPool
{
private:
    static constexpr size_t actual_block_size =
        (block_size >= sizeof(void*)) ? block_size : sizeof(void*);
    struct Node
    {
        char data[actual_block_size];
        Node* next;
    };
    std::mutex mtx_;
    Node* free_list_;
    struct Chunk { Chunk* next; };
    Chunk* chunk_list_;

public:
    MutexMemoryPool() : free_list_(nullptr), chunk_list_(nullptr) {}
    ~MutexMemoryPool() { clear(); }
    MutexMemoryPool(const MutexMemoryPool&) = delete;
    MutexMemoryPool& operator=(const MutexMemoryPool&) = delete;

    void* allocate() noexcept
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!free_list_) return nullptr;
        Node* node = free_list_;
        free_list_ = node->next;
        std::memset(node->data, 0, actual_block_size);
        return node->data;
    }

    void deallocate(void* ptr) noexcept
    {
        if (!ptr) return;
        Node* node = reinterpret_cast<Node*>(ptr);
        std::memset(node->data, 0, actual_block_size);
        std::lock_guard<std::mutex> lock(mtx_);
        node->next = free_list_;
        free_list_ = node;
    }

    void expand(size_t num_blocks = 1024)
    {
        size_t alloc_size = sizeof(Chunk) + num_blocks * sizeof(Node);
        void* raw = ::operator new[](alloc_size, std::align_val_t{ alignof(Chunk) });
        Chunk* chunk = new (raw) Chunk{ nullptr };
        Node* blocks = reinterpret_cast<Node*>(
            reinterpret_cast<char*>(raw) + sizeof(Chunk));
        for (size_t i = 0; i < num_blocks; ++i)
            new (&blocks[i]) Node();
        std::lock_guard<std::mutex> lock(mtx_);
        chunk->next = chunk_list_;
        chunk_list_ = chunk;
        for (size_t i = 0; i < num_blocks - 1; ++i)
            blocks[i].next = &blocks[i + 1];
        blocks[num_blocks - 1].next = free_list_;
        free_list_ = &blocks[0];
    }

    void clear() noexcept
    {
        std::lock_guard<std::mutex> lock(mtx_);
        Chunk* cur = chunk_list_;
        while (cur)
        {
            Chunk* nxt = cur->next;
            ::operator delete[](reinterpret_cast<void*>(cur),
                std::align_val_t{ alignof(Chunk) });
            cur = nxt;
        }
        chunk_list_ = nullptr;
        free_list_ = nullptr;
    }
};

// =============== 性能测试模板 ===============
template <typename Pool>
double run_benchmark(Pool& pool, int num_threads, int ops_per_thread)
{
    pool.expand(num_threads * ops_per_thread + 4096); // 预分配充足

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&pool, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; ++i)
            {
                void* p = pool.allocate();
                if (p)
                {
                    // 模拟轻量使用
                    std::memset(p, 'A', sizeof(char));
                    pool.deallocate(p);
                }
            }
            });
    }
    for (auto& th : threads) th.join();

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();
    double ops = num_threads * ops_per_thread;
    return ops / elapsed_s; // 吞吐量（ops/s）
}

int main()
{
    const int THREAD_COUNTS[] = { 1, 2, 4, 8, 16, 32 };
    const int OPS_PER_THREAD = 200000;
    const int BLOCK_SIZE = 128;

    std::cout << "Performance comparison: Lock vs LockFree memory pool\n";
    std::cout << "Block size: " << BLOCK_SIZE << " bytes\n";
    std::cout << "Operations per thread: " << OPS_PER_THREAD << "\n\n";
    std::cout << std::left << std::setw(12) << "Threads"
        << std::setw(18) << "Mutex (ops/s)"
        << std::setw(18) << "LockFree (ops/s)"
        << std::setw(14) << "Speedup" << "\n";
    std::cout << std::string(62, '-') << "\n";

    for (int nt : THREAD_COUNTS)
    {
        MutexMemoryPool<BLOCK_SIZE> mutex_pool;
        double mutex_throughput = run_benchmark(mutex_pool, nt, OPS_PER_THREAD);

        memory_pool::LockFreeMemoryPool<BLOCK_SIZE> lockfree_pool;
        double lockfree_throughput = run_benchmark(lockfree_pool, nt, OPS_PER_THREAD);

        double speedup = lockfree_throughput / mutex_throughput;

        std::cout << std::setw(12) << nt
            << std::setw(18) << std::fixed << std::setprecision(0) << mutex_throughput
            << std::setw(18) << lockfree_throughput
            << std::setw(14) << std::setprecision(2) << speedup << "\n";
    }

    return 0;
}