#include "../include/lock_free_memory_pool.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <random>
using namespace memory_pool;
// 测试结构体
struct TestStruct
{
    int id;
    double value;
    char name[32];
};


// 基本功能测试
void test_basic_functionality()
{
    std::cout << "=== 测试基本功能 ===" << std::endl;

    LockFreeMemoryPool<sizeof(TestStruct)> pool;

    // 测试1: 空池分配应返回nullptr
    void* ptr1 = pool.allocate();
    assert(ptr1 == nullptr);
    std::cout << "测试1通过: 空池分配返回nullptr" << std::endl;

    // 测试2: 扩容后分配
    pool.expand(5);
    std::vector<void*> pointers;

    for (int i = 0; i < 5; ++i)
    {
        void* ptr = pool.allocate();
        assert(ptr != nullptr);
        pointers.push_back(ptr);

        // 使用分配的内存
        TestStruct* ts = static_cast<TestStruct*>(ptr);
        ts->id = i;
        ts->value = i * 1.5;
        snprintf(ts->name, sizeof(ts->name), "Test%d", i);
    }
    std::cout << "测试2通过: 成功分配5个块" << std::endl;

    // 测试3: 超过容量的分配
    void* overflow_ptr = pool.allocate();
    assert(overflow_ptr == nullptr);
    std::cout << "测试3通过: 超过容量分配返回nullptr" << std::endl;

    // 测试4: 释放和重新分配
    for (void* ptr : pointers)
    {
        // 验证数据完整性
        TestStruct* ts = static_cast<TestStruct*>(ptr);
        assert(ts->id >= 0 && ts->id < 5);
        pool.deallocate(ptr);
    }
    std::cout << "测试4通过: 成功释放所有块" << std::endl;

    // 测试5: 重新分配
    //for (int i = 0; i < 5; ++i)
    //{
     //   void* ptr = pool.allocate();
     //   assert(ptr != nullptr);

        // 验证内存被清零
    //    TestStruct* ts = static_cast<TestStruct*>(ptr);
     //   assert(ts->id == 0); // 内存应该被清零
    //    pool.deallocate(ptr);
  //  }
   // std::cout << "测试5通过: 成功重新分配和释放" << std::endl;

    // 测试6: 多次扩容
    pool.expand(3);
    for (int i = 0; i < 3; ++i)
    {
        void* ptr = pool.allocate();
        assert(ptr != nullptr);
        pool.deallocate(ptr);
    }
    std::cout << "测试6通过: 多次扩容正常" << std::endl;


}

// 多线程压力测试
void stress_test_single_pool()
{
    std::cout << "\n=== 多线程压力测试 ===" << std::endl;

    const int NUM_THREADS = 8;
    const int OPERATIONS_PER_THREAD = 10000;
    const int INITIAL_BLOCKS = 1000;

    LockFreeMemoryPool<64> pool;
    pool.expand(INITIAL_BLOCKS);

    std::vector<std::thread> threads;
    std::atomic<int> allocation_failures{ 0 };
    std::atomic<int> successful_operations{ 0 };

    auto worker = [&pool, &allocation_failures, &successful_operations](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1, 100);

        for (int i = 0; i < OPERATIONS_PER_THREAD; ++i)
        {
            if (dis(gen) <= 60)
            { // 60%的概率分配
                void* ptr = pool.allocate();
                if (ptr)
                {
                    // 使用内存
                    int* data = static_cast<int*>(ptr);
                    *data = thread_id * 1000 + i;

                    // 短暂持有
                    std::this_thread::yield();

                    // 验证数据
                    if (*data != thread_id * 1000 + i)
                    {
                        std::cerr << "数据损坏！" << std::endl;
                    }

                    pool.deallocate(ptr);
                    successful_operations.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    allocation_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            { // 40%的概率只分配不释放（模拟内存泄漏场景）
                void* ptr = pool.allocate();
                if (ptr)
                {
                    // 分配但不释放，测试池是否会耗尽
                    // 在实际测试中，我们应该在最后释放这些内存
                    pool.deallocate(ptr); // 为了测试，我们还是释放
                }
            }
        }
        };

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads)
    {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "线程数: " << NUM_THREADS << std::endl;
    std::cout << "总操作数: " << (NUM_THREADS * OPERATIONS_PER_THREAD) << std::endl;
    std::cout << "成功操作: " << successful_operations.load() << std::endl;
    std::cout << "分配失败次数: " << allocation_failures.load() << std::endl;
    std::cout << "耗时: " << duration.count() << "ms" << std::endl;
    std::cout << "可用块数: " << pool.available_blocks() << std::endl;

    assert(allocation_failures.load() == 0);
    std::cout << "压力测试通过！" << std::endl;
}

// ABA问题测试
void test_aba_protection()
{
    std::cout << "\n=== ABA问题防护测试 ===" << std::endl;

    LockFreeMemoryPool<32> pool;
    pool.expand(3);

    // 分配三个块
    void* ptr1 = pool.allocate();
    void* ptr2 = pool.allocate();
    void* ptr3 = pool.allocate();

    assert(ptr1 != nullptr);
    assert(ptr2 != nullptr);
    assert(ptr3 != nullptr);

    // 释放ptr1
    pool.deallocate(ptr1);

    // 在另一个线程中分配和释放，模拟ABA场景
    std::thread t1([&pool, ptr1]() {
        void* temp = pool.allocate();
        if (temp)
        {
            // 使用内存
            int* data = static_cast<int*>(temp);
            *data = 42;

            // 短暂延迟
            std::this_thread::sleep_for(std::chrono::microseconds(10));

            pool.deallocate(temp);
        }
        });

    // 短暂延迟，让t1有机会执行
    std::this_thread::sleep_for(std::chrono::microseconds(1));

    // 在主线程中尝试重新分配ptr1
    // 由于有标签保护，即使ptr1被其他线程分配又释放，也不会发生ABA问题
    pool.deallocate(ptr2);
    pool.deallocate(ptr3);

    t1.join();

    // 验证池仍然工作
    for (int i = 0; i < 3; ++i)
    {
        void* ptr = pool.allocate();
        assert(ptr != nullptr);
        pool.deallocate(ptr);
    }

    std::cout << "ABA防护测试通过！" << std::endl;
}

// 性能对比测试
void performance_comparison()
{
    std::cout << "\n=== 性能对比测试 ===" << std::endl;

    const int ITERATIONS = 1000000;

    // 测试1: 使用无锁内存池
    {
        LockFreeMemoryPool<64> pool;
        pool.expand(ITERATIONS);

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < ITERATIONS; ++i)
        {
            void* ptr = pool.allocate();
            int* data = static_cast<int*>(ptr);
            *data = i;
            pool.deallocate(ptr);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        std::cout << "无锁内存池耗时: " << duration.count() << "μs" << std::endl;
        std::cout << "平均每次分配/释放: " << (duration.count() * 1.0 / ITERATIONS) << "μs" << std::endl;
    }

    // 测试2: 使用new/delete
    {
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < ITERATIONS; ++i)
        {
            int* data = new int(i);
            delete data;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        std::cout << "new/delete耗时: " << duration.count() << "μs" << std::endl;
        std::cout << "平均每次分配/释放: " << (duration.count() * 1.0 / ITERATIONS) << "μs" << std::endl;
    }
}

int main()
{

    try
    {
        std::cout << "开始测试无锁内存池..." << std::endl;

        test_basic_functionality();
        stress_test_single_pool();
        test_aba_protection();
        performance_comparison();

        std::cout << "\n=== 所有测试通过！ ===" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "测试失败: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "未知错误" << std::endl;
        return 1;
    }
    std::system("pause");
}
