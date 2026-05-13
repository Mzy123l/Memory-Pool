#include <vector>
#include <array>
#include <memory>
#include <cstddef>
#include <type_traits>
#include <algorithm>
#include <utility>
#include <exception>
#include <tuple>
#include <initializer_list>
#include <compare>
#include <cassert>
#include <stdexcept>

namespace container
{
    /*
    ◦ @brief 自定义块大小的deque类
    ◦ @brief 更佳的缓存友好性
    ◦ @param 参数1 数据类型T
    ◦ @param 参数2 单个block的数据个数(非字节数)
    ◦ @param 参数3 数据类型T的内存分配器
    */
    template <typename T, std::size_t block_size = 1024, typename Allocator = std::allocator<T>>
    class deque
    {
    private:
        // 使用对齐存储确保正确对齐
        using aligned_storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
        using block_type = std::array<aligned_storage, block_size>;

        // 块分配器类型
        using block_allocator_type = typename std::allocator_traits<Allocator>::
            template rebind_alloc<block_type>;


        // 元素分配器
        Allocator _alloc;
        // 块分配器
        block_allocator_type _block_alloc;


        

        // 前向块存储（顺序向前扩展）
        std::vector<block_type*> _front_blocks;
        // 后向块存储（顺序向后扩展）
        std::vector<block_type*> _back_blocks;

        // 前向块中的元素起始偏移
        std::size_t _start_offset = 0;
        // 后向块中的元素结束偏移
        std::size_t _finish_offset = 0;
        // 大小
        std::size_t _length = 0;

    private:
        enum class Area // 在前向块还是后向块
        {
            FRONT = -1,
            BACK = 1
        };
    private:
        // 前向声明迭代器类
        template <typename U>
        class iterator_base;
    public:
        using iterator = iterator_base<T>;
        using const_iterator = iterator_base<const T>;

    public:
        /// @brief 默认构造函数
        /// @param alloc 自定义分配器
        explicit deque(const Allocator& alloc = Allocator()) noexcept
            : _alloc(alloc)
            , _block_alloc(alloc)
        {}

        /// @brief 初始化列表构造
        /// @param alloc 自定义分配器
        deque(std::initializer_list<T> init, const Allocator& alloc = Allocator())
            : _alloc(alloc)
            , _block_alloc(alloc)
        {
            std::size_t count = init.size();
            if (count == 0) return;

            std::size_t block_count = (count + block_size - 1) / block_size;
            _back_blocks.reserve(block_count);

            std::size_t idx = 0;
            auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);

            for (const auto& val : init)
            {
                T* pos = reinterpret_cast<T*>(&(*new_block)[idx % block_size]);
                std::allocator_traits<Allocator>::construct(_alloc, pos, val);
                ++idx;

                if (idx % block_size == 0 && idx < count)
                {
                    _back_blocks.push_back(new_block);
                    new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                }
            }

            if (idx % block_size != 0)
            {
                _back_blocks.push_back(new_block);
                _finish_offset = idx % block_size;
            }
            else
            {
                _back_blocks.push_back(new_block);
                _finish_offset = 0;
            }

            _length = count;
        }
        deque& operator=(std::initializer_list<T> init)
        {
            clear();
            std::size_t count = init.size();
            if (count == 0) return;

            std::size_t block_count = (count + block_size - 1) / block_size;
            _back_blocks.reserve(block_count);

            std::size_t idx = 0;
            auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);

            for (const auto& val : init)
            {
                T* pos = reinterpret_cast<T*>(&(*new_block)[idx % block_size]);
                std::allocator_traits<Allocator>::construct(_alloc, pos, val);
                ++idx;

                if (idx % block_size == 0 && idx < count)
                {
                    _back_blocks.push_back(new_block);
                    new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                }
            }

            if (idx % block_size != 0)
            {
                _back_blocks.push_back(new_block);
                _finish_offset = idx % block_size;
            }
            else
            {
                _back_blocks.push_back(new_block);
                _finish_offset = 0;
            }

            _length = count;
            return *this;
        }

        // 拷贝构造函数
        deque(const deque& other)
            : _alloc(std::allocator_traits<Allocator>::select_on_container_copy_construction(other._alloc))
            , _block_alloc(_alloc)
            , _start_offset(other._start_offset)
            , _finish_offset(other._finish_offset)
            , _length(other._length)
        {
            // 深拷贝前向块
            _front_blocks.reserve(other._front_blocks.size());
            for (auto block : other._front_blocks)
            {
                if (block)
                {
                    auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                    __deep_copy_block(block, new_block);
                    _front_blocks.push_back(new_block);
                }
                else
                {
                    _front_blocks.push_back(nullptr);
                }
            }

            // 深拷贝后向块
            _back_blocks.reserve(other._back_blocks.size());
            for (auto block : other._back_blocks)
            {
                if (block)
                {
                    auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                    __deep_copy_block(block, new_block);
                    _back_blocks.push_back(new_block);
                }
                else
                {
                    _back_blocks.push_back(nullptr);
                }
            }
        }

        // 拷贝赋值运算符
        deque& operator=(const deque& other)
        {
            if (this != &other)
            {
                // 释放原有资源
                for (auto block : _front_blocks)
                {
                    if (block)
                    {
                        for (std::size_t i = 0; i < block_size; ++i)
                        {
                            std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                        }
                        std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                    }
                }
                for (auto block : _back_blocks)
                {
                    if (block)
                    {
                        for (std::size_t i = 0; i < block_size; ++i)
                        {
                            std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                        }
                        std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                    }
                }

                _alloc = other._alloc;
                _block_alloc = other._block_alloc;
                _start_offset = other._start_offset;
                _finish_offset = other._finish_offset;
                _length = other._length;

                // 清空现有块
                _front_blocks.clear();
                _back_blocks.clear();

                // 深拷贝前向块
                _front_blocks.reserve(other._front_blocks.size());
                for (auto block : other._front_blocks)
                {
                    if (block)
                    {
                        auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                        __deep_copy_block(block, new_block);
                        _front_blocks.push_back(new_block);
                    }
                    else
                    {
                        _front_blocks.push_back(nullptr);
                    }
                }

                // 深拷贝后向块
                _back_blocks.reserve(other._back_blocks.size());
                for (auto block : other._back_blocks)
                {
                    if (block)
                    {
                        auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                        __deep_copy_block(block, new_block);
                        _back_blocks.push_back(new_block);
                    }
                    else
                    {
                        _back_blocks.push_back(nullptr);
                    }
                }
            }
            return *this;
        }

        // 移动构造函数
        deque(deque&& other) noexcept
            : _alloc(std::move(other._alloc))
            , _block_alloc(std::move(other._block_alloc))
            , _front_blocks(std::move(other._front_blocks))
            , _back_blocks(std::move(other._back_blocks))
            , _start_offset(other._start_offset)
            , _finish_offset(other._finish_offset)
            , _length(other._length)
        {
            other._front_blocks.clear();
            other._back_blocks.clear();
            other._start_offset = 0;
            other._finish_offset = 0;
            other._length = 0;
        }

        // 移动赋值运算符
        deque& operator=(deque&& other) noexcept
        {
            if (this != &other)
            {
                // 释放原有资源
                for (auto block : _front_blocks)
                {
                    if (block)
                    {
                        for (std::size_t i = 0; i < block_size; ++i)
                        {
                            std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                        }
                        std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                    }
                }
                for (auto block : _back_blocks)
                {
                    if (block)
                    {
                        for (std::size_t i = 0; i < block_size; ++i)
                        {
                            std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                        }
                        std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                    }
                }

                _alloc = std::move(other._alloc);
                _block_alloc = std::move(other._block_alloc);
                _front_blocks = std::move(other._front_blocks);
                _back_blocks = std::move(other._back_blocks);
                _start_offset = other._start_offset;
                _finish_offset = other._finish_offset;
                _length = other._length;

                other._front_blocks.clear();
                other._back_blocks.clear();
                other._start_offset = 0;
                other._finish_offset = 0;
                other._length = 0;
            }
            return *this;
        }

        // 析构函数
        ~deque()
        {
            for (auto block : _front_blocks)
            {
                if (block)
                {
                    for (std::size_t i = 0; i < block_size; ++i)
                    {
                        std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                    }
                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                }
            }
            for (auto block : _back_blocks)
            {
                if (block)
                {
                    for (std::size_t i = 0; i < block_size; ++i)
                    {
                        std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                    }
                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                }
            }
        }
        // 清空容器
        void clear()noexcept
        {
            for (auto block : _front_blocks)
            {
                if (block)
                {
                    for (std::size_t i = 0; i < block_size; ++i)
                    {
                        std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                    }
                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                }
            }
            for (auto block : _back_blocks)
            {
                if (block)
                {
                    for (std::size_t i = 0; i < block_size; ++i)
                    {
                        std::allocator_traits<Allocator>::destroy(_alloc, &((*block)[i]));
                    }
                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, block, 1);
                }
            }
            _front_blocks.clear();
            _back_blocks.clear();
            _length = 0;
            _finish_offset = 0;
            _start_offset = 0;
        }

        // 元素访问
        T& operator[](std::size_t pos)
        {
            // 计算在前向块还是后向块
            std::size_t front_capacity = (_front_blocks.size() - 1) * block_size + _start_offset;

            if (pos < front_capacity)
            {
                // 在前向块中
                std::size_t total_pos = block_size - _start_offset + pos;
                std::size_t block_idx = _front_blocks.size() - 1 - total_pos / block_size;
                std::size_t elem_idx = block_size - total_pos % block_size - 1;
                return reinterpret_cast<T&>((*_front_blocks[block_idx])[elem_idx]);
            }
            else
            {
                // 在后向块中
                std::size_t back_pos = pos - front_capacity;
                std::size_t block_idx = back_pos / block_size;
                std::size_t elem_idx = back_pos % block_size;
                return reinterpret_cast<T&>((*_back_blocks[block_idx])[elem_idx]);
            }

        }
        const T& operator[](std::size_t pos) const
        {
            
            // 计算在前向块还是后向块
            std::size_t front_capacity = (_front_blocks.size() - 1) * block_size + _start_offset;

            if (pos < front_capacity)
            {
                // 在前向块中
                std::size_t total_pos = block_size - _start_offset + pos;
                std::size_t block_idx = _front_blocks.size() - 1 - total_pos / block_size;
                std::size_t elem_idx = block_size - total_pos % block_size - 1;
                return reinterpret_cast<T&>((*_front_blocks[block_idx])[elem_idx]);
            }
            else
            {
                // 在后向块中
                std::size_t back_pos = pos - front_capacity;
                std::size_t block_idx = back_pos / block_size;
                std::size_t elem_idx = back_pos % block_size;
                return reinterpret_cast<T&>((*_back_blocks[block_idx])[elem_idx]);
            }

        }

        // 带检查的元素访问
        T& at(std::size_t pos)
        {
            if (pos >= _length)
                throw std::out_of_range("deque::at: pos >= size()");
            return (*this)[pos];
        }
        const T& at(std::size_t pos) const
        {
            if (pos >= _length)
                throw std::out_of_range("deque::at: pos >= size()");
            return (*this)[pos];
        }

        // 访问头元素
        T& front()
        {
            return reinterpret_cast<T&>((*_front_blocks[_front_blocks.size() - 1])[_start_offset - 1]);
        }
        const T& front() const
        {
            return reinterpret_cast<const T&>((*_front_blocks[_front_blocks.size() - 1])[_start_offset - 1]);
        }

        // 访问最后一个元素
        T& back()
        {
            return reinterpret_cast<T&>((*_back_blocks[_back_blocks.size() - 1])[_finish_offset - 1]);
        }
        const T& back() const
        {
            return reinterpret_cast<const T&>((*_back_blocks[_back_blocks.size() - 1])[_finish_offset - 1]);
        }
        // 是否为空
        bool empty() const
        {
            return _length == 0;
        }

        // 返回元素数量
        std::size_t size() const
        {
            return _length;
        }

        // 收缩到合适大小
        void shrink_to_fit()
        {
            _front_blocks.shrink_to_fit();
            _back_blocks.shrink_to_fit();
        }

        // 尾插, 原位构造
        template <typename... Args>
        T& emplace_back(Args&&... args)
        {
            // 处理空容器和到达block尾部的情况
            if (_finish_offset == block_size)
            {
                _finish_offset = 0;
            }
            if (_finish_offset == 0)
            {
                auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                _back_blocks.push_back(new_block);
            }
            auto _block = _back_blocks[_back_blocks.size() - 1];
            T* dst_elem = reinterpret_cast<T*>(&(*_block)[_finish_offset]);
            std::allocator_traits<Allocator>::construct(_alloc, dst_elem, std::forward<Args>(args)...);
            ++_finish_offset;
            return back();
        }

        // 尾插
        template <typename U>
            requires std::is_convertible_v<U ,T>
        void push_back(U&& u)
        {
            emplace_back(std::forward<U>(u));
        }

        // 尾删
        void pop_back()
        {
            if (_back_blocks.empty() && _front_blocks.empty()) return;
            else if (_back_blocks.empty())
            {
                std::allocator_traits<Allocator>::destroy(_alloc, &((*_front_blocks[0])[0]));
                for (std::size_t i = 0; i < _front_blocks.size() - 1; ++i)
                {
                    std::memmove(&((*_front_blocks[i])[0]), &((*_front_blocks[i])[1]), sizeof(block_type) - sizeof(block_type) / block_size);
                }
                _finish_offset = 0;
            }
            else
            {
                if (_finish_offset % block_size == 0)
                {
                    if (_back_blocks.size() != 1)
                    {
                        _finish_offset = block_size - 1;
                    }
                    else
                    {
                        _finish_offset = 0;
                    }
                    std::allocator_traits<block_allocator_type>::deallocte(_block_alloc, _back_blocks[_back_blocks.size() - 1], 1);
                }
                else
                {
                    --_finish_offset;
                }
                std::allocator_traits<Allocator>::destroy(_alloc, &((*_back_blocks[_back_blocks.size() - 1])[_finish_offset]));
            }
            --_length;
        }

        // 头插, 原位构造
        template <typename... Args>
        T& emplace_front(Args&&... args)
        {
            // 处理空容器和到达block尾部的情况
            if (_start_offset == block_size)
            {
                _start_offset = 0;
            }
            if (_start_offset == 0)
            {
                auto* new_block = std::allocator_traits<block_allocator_type>::allocate(_block_alloc, 1);
                _front_blocks.push_back(new_block);
            }
            auto _block = _front_blocks[_front_blocks.size() - 1];
            T* dst_elem = reinterpret_cast<T*>(&(*_block)[_start_offset]);
            std::allocator_traits<Allocator>::construct(_alloc, dst_elem, std::forward<Args>(args)...);
            ++_start_offset;
            return front();
        }

        // 头插
        template <typename U>
            requires std::is_convertible_v<U, T>
        void push_front(U&& u)
        {
            emplace_front(std::forward<U>(u));
        }

        // 头删
        void pop_front()
        {
            if (_back_blocks.empty() && _front_blocks.empty()) return;
            else if (_front_blocks.empty())
            {
                std::allocator_traits<Allocator>::destroy(_alloc, &((*_back_blocks[0])[0]));
                for (std::size_t i = 0; i < _front_blocks.size() - 1; ++i)
                {
                    std::memmove(&((*_back_blocks[i])[0]), &((*_back_blocks[i])[1]), sizeof(block_type) - sizeof(block_type) / block_size);
                }
                _start_offset = 0;
            }
            else
            {
                if (_start_offset % block_size == 0)
                {
                    if (_front_blocks.size() != 1)
                    {
                        _start_offset = block_size - 1;
                    }
                    else
                    {
                        _start_offset = 0;
                    }
                    std::allocator_traits<block_allocator_type>::deallocte(_block_alloc, _front_blocks[_front_blocks.size() - 1], 1);
                }
                else
                {
                    --_start_offset;
                }
                std::allocator_traits<Allocator>::destroy(_alloc, &((*_front_blocks[_front_blocks.size() - 1])[_start_offset]));
            }
            --_length;
        }
        /// @brief 原位构造插入
        /// @param pos 插入位置迭代器
        /// @param args... 构造元素
        /// @return 指向插入元素的迭代器
        template<class... Args>
        iterator emplace(const_iterator pos, Args&&... args)
        {
            T* insert_pos;
            if (pos.__area == Area::FRONT)
            {
                insert_pos = reinterpret_cast<T*>(&(*(_front_blocks[pos.__current_block_idx]))[pos.__current_elem_idx]);
                if (_start_offset == block_size - 1)
                {
                    auto* new_block = std::allocator_traits<block_allocator_type>::allocator(_block_alloc, 1);
                    _front_blocks.push_back(new_block);
                    _start_offset = 0;
                }
                else
                {
                    ++_start_offset;
                }
                for (std::size_t i = _front_blocks.size() - 1; i > pos.__current_block_idx; --i)
                {
                    std::memmove(&(*(_front_blocks[i])[1]), &(*(_front_blocks[i])[0]), sizeof(block_type) - sizeof(block_type) / block_size);
                    std::memmove(&(*(_front_blocks[i])[0]), &(*(_front_blocks[i - 1])[block_size - 1]), sizeof(block_type) / block_size);
                }
                if (pos.__current_elem_idx != block_size - 1)
                {
                    std::memmove(&(*(_front_blocks[pos.__current_block_idx])[pos.__current_elem_idx + 1]), &(*(_front_blocks[pos.__current_block_idx])[pos.__current_elem_idx]),
                        sizeof(block_type) / block_size * (block_size - pos.__current_elem_idx - 1));
                }

            }
            else
            {
                insert_pos = reinterpret_cast<T*>(&(*(_back_blocks[pos.__current_block_idx]))[pos.__current_elem_idx]);
                if (_finish_offset == block_size - 1)
                {
                    auto* new_block = std::allocator_traits<block_allocator_type>::allocator(_block_alloc, 1);
                    _back_blocks.push_back(new_block);
                    _finish_offset = 0;
                }
                else
                {
                    ++_finish_offset;
                }
                for (std::size_t i = _back_blocks.size() - 1; i > pos.__current_block_idx; --i)
                {
                    std::memmove(&(*(_back_blocks[i])[1]), &(*(_back_blocks[i])[0]), sizeof(block_type) - sizeof(block_type) / block_size);
                    std::memmove(&(*(_back_blocks[i])[0]), &(*(_back_blocks[i - 1])[block_size - 1]), sizeof(block_type) / block_size);
                }
                if (pos.__current_elem_idx != block_size - 1)
                {
                    std::memmove(&(*(_back_blocks[pos.__current_block_idx])[pos.__current_elem_idx + 1]), &(*(_back_blocks[pos.__current_block_idx])[pos.__current_elem_idx]),
                        sizeof(block_type) / block_size * (block_size - pos.__current_elem_idx - 1));
                }
            }
            std::allocator_traits<Allocator>::construct(_alloc, insert_pos, std::forward<Args>(args)...);
            ++_length;
            return iterator(this, pos.__current_block_idx, pos.__current_elem_idx, pos.__area);
        }

        // 在指定位置插入
        /// @brief 原位构造插入
        /// @param pos 插入位置迭代器
        /// @param value 插入元素
        /// @return 指向插入元素的迭代器
        template <typename U>
        iterator insert(const_iterator pos, U&& value)
        {
            emplace(std::forward<U>(value));
        }

        // 擦除
        iterator erase(const_iterator pos)
        {
            std::size_t __block_idx_;
            std::size_t __elem_idx_;

            Area __area_;

            if (pos.__area == Area::FRONT)
            {

                T* erase_pos = reinterpret_cast<T*>(&((*_front_blocks[pos.__current_block_idx])[pos.__current_elem_idx]));

                std::allocator_traits<Allocator>::destroy(_alloc, erase_pos);

                std::memmove(erase_pos, &(*(_front_blocks[pos.__current_block_idx])[pos.__current_elem_idx + 1]),
                    sizeof(block_type) / block_size * (block_size - 1 - pos.__current_elem_idx));

                for (std::size_t i = pos.__current_block_idx; i < _front_blocks.size() - 1; ++i)
                {

                    std::memmove(&((*_front_blocks[i])[block_size - 1]), &((*_front_blocks[i + 1])[0]), sizeof(block_type) / block_size);
                    std::memmove(&((*_front_blocks[i + 1])[0]), &((*_front_blocks[i + 1])[1]), sizeof(block_type) - sizeof(block_type) / block_size);
                }

                if (pos.__current_elem_idx == 0 && pos.__current_block_idx == 0)
                {

                    __block_idx_ = 0;
                    __elem_idx_ = 0;
                    __area_ = Area::BACK;

                }

                else if (pos.__current_elem_idx == 0)
                {
                    __block_idx_ = pos.__current_block_idx - 1;
                    __elem_idx_ = block_size - 1;
                    __area_ = Area::FRONT;

                }
                else
                {
                    __block_idx_ = pos.__current_block_idx;
                    __elem_idx_ = pos.__current_elem_idx - 1;
                    __area_ = Area::FRONT;
                }
                if (_start_offset % block_size == 0)
                {

                    if (_back_blocks.size() != 1)
                    {
                        _start_offset = block_size - 1;
                    }
                    else
                    {
                        _start_offset = 0;
                    }

                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, _front_blocks[_front_blocks.size() - 1], 1);

                    _front_blocks.pop_back();

                }

                else
                {
                    --_start_offset;
                }
            }

            else
            {

                T* erase_pos = reinterpret_cast<T*>(&((*_back_blocks[pos.__current_block_idx])[pos.__current_elem_idx]));
                std::allocator_traits<Allocator>::destroy(_alloc, erase_pos);
                std::memmove(erase_pos, &(*(_back_blocks[pos.__current_block_idx])[pos.__current_elem_idx + 1]), 
                    sizeof(block_type) / block_size * (block_size - 1 - pos.__current_elem_idx));

                for (std::size_t i = pos.__current_block_idx; i < _back_blocks.size() - 1; ++i)
                {
                    std::memmove(&((*_back_blocks[i])[block_size - 1]), &((*_back_blocks[i + 1])[0]), sizeof(block_type) / block_size);
                    std::memmove(&((*_back_blocks[i + 1])[0]), &((*_back_blocks[i + 1])[1]), sizeof(block_type) - sizeof(block_type) / block_size);
                }
                __block_idx_ = pos.__current_block_idx;
                __elem_idx_ = pos.__current_elem_idx;
                __area_ = Area::BACK;

                if (_finish_offset % block_size == 0)             
                {
                    if (_back_blocks.size() != 1)
                    {
                        _finish_offset = block_size - 1;
                    }
                    else
                    {
                        _finish_offset = 0;
                    }

                    std::allocator_traits<block_allocator_type>::deallocate(_block_alloc, _back_blocks[_back_blocks.size() - 1], 1);
                    _back_blocks.pop_back();
                }
                else
                {
                    --_finish_offset;
                }

            }

            --_length;

            return iterator(this, __block_idx_, __elem_idx_, __area_);
        }

        // 交换
        void swap(deque& other) noexcept
        {
            _front_blocks.swap(other._front_blocks);
            _back_blocks.swap(other._back_blocks);
            std::swap(_alloc, other._alloc);
            std::swap(_block_alloc, other._block_alloc);
            std::swap(_start_offset, other._start_offset);
            std::swap(_finish_offset, other._finish_offset);
            std::swap(_length, other._length);
        }

    private:
        template <typename U>
        friend class iterator_base;

        // 迭代器
        template <typename U>
        class iterator_base
        {
        private:

            deque* __deque;
            std::size_t __current_block_idx = 0;
            std::size_t __current_elem_idx = 0;
            Area __area;
            using difference_type = ptrdiff_t;

        public:
            // 友元声明
            friend class deque;

            template <typename U>
            friend class iterator_base;

            // 默认构造函数
            iterator_base()noexcept {}
        private:
            explicit iterator_base(deque* __deque_, std::size_t __current_block_idx_, std::size_t __current_elem_idx_, Area __area_) noexcept :
                __deque(__deque_), __current_block_idx(__current_block_idx_), __current_elem_idx(__current_elem_idx_), __area(__area_) {}

        public:
            // 拷贝构造函数
            iterator_base(const iterator_base& other) noexcept: __deque(other.__deque), 
                __current_block_idx(other.__current_block_idx), __current_elem_idx(other.__current_elem_idx), __area(other.__area) {}

            // 用非常量迭代器构造常量迭代器
            template <typename OtherU>
                requires (std::is_same_v<U, const T> && std::is_same_v<OtherU, T>)
            iterator_base(const iterator_base<OtherU>& other) noexcept: __deque(other.__deque),
                __current_block_idx(other.__current_block_idx), __current_elem_idx(other.__current_elem_idx), __area(other.__area){}

            // 拷贝赋值运算符
            iterator_base& operator=(const iterator_base& other) noexcept
            {
                if (this != &other)
                {
                    __deque = other.__deque;
                    __current_block_idx = other.__current_block_idx;
                    __current_elem_idx = other.__current_elem_idx;
                    __area = other.__area;
                }
                return *this;
            }
            // 用非常量迭代器给常量迭代器赋值
            template <typename OtherU>
                requires (std::is_same_v<U, const T>&& std::is_same_v<OtherU, T>)
            iterator_base& operator=(const iterator_base<OtherU>& other) noexcept
            {
                if (this != &other)
                {
                    __deque = other.__deque;
                    __current_block_idx = other.__current_block_idx;
                    __current_elem_idx = other.__current_elem_idx;
                    __area = other.__area;
                }
                return *this;
            }
           
            // 解引用
            U& operator*() const
            {
                if (__area == Area::FRONT)
                {
                    return reinterpret_cast<U&>(*(__deque->_front_blocks[__current_block_idx])[__current_elem_idx]);
                }
                else
                {
                    return reinterpret_cast<U&>(*(__deque->_back_blocks[__current_block_idx])[__current_elem_idx]);
                }
            }

            // ->运算符
            U* operator->() const
            {
                if (__area == Area::FRONT)
                {
                    return reinterpret_cast<U*>(&(*(__deque->_front_blocks[__current_block_idx])[__current_elem_idx]));
                }
                else
                {
                    return reinterpret_cast<U&>(&(*(__deque->_back_blocks[__current_block_idx])[__current_elem_idx]));
                }
            }
            // 重载下标偏移量访问
            U& operator[](std::size_t pos) const
            {
                return *this + pos;
            }

            // 前置递增
            iterator_base& operator++()
            {
                if (__area == Area::FRONT)
                {
                    if (__current_elem_idx != 0)
                    {
                        --__current_elem_idx;
                    }
                    else if (__current_block_idx != 0)
                    {
                        --__current_block_idx;
                        __current_elem_idx = block_size - 1;
                    }
                    else
                    {
                        __area = Area::BACK;
                        __current_elem_idx = __current_block_idx = 0;
                    }
                }
                else
                {
                    if (__current_elem_idx != block_size - 1)
                    {
                        ++__current_elem_idx;
                    }
                    else
                    {
                        ++__current_block_idx;
                        __current_elem_idx = 0;
                    }
                }
                return *this;
            }

            // 后置递增
            iterator_base operator++(int)
            {
                auto temp = *this;
                ++(*this);
                return temp;
            }

            // 前置递减
            iterator_base& operator--()
            {
                if (__area == Area::FRONT)
                {
                    if (__current_elem_idx != block_size - 1)
                    {
                        ++__current_elem_idx;
                    }
                    else
                    {
                        ++__current_block_idx;
                        __current_elem_idx = 0;
                    }
                }
                else
                {
                    if (__current_elem_idx != 0)
                    {
                        --__current_elem_idx;
                    }
                    else if (__current_block_idx != 0)
                    {
                        --__current_block_idx;
                        __current_elem_idx = block_size - 1;
                    }
                    else
                    {
                        __area = Area::FRONT;
                        __current_elem_idx = __current_block_idx = 0;
                    }
                }
                return *this;
            }

            // 后置递减
            iterator_base operator--(int)
            {
                auto temp = *this;
                --(*this);
                return temp;
            }

            // 加法运算
            iterator_base operator+(difference_type n) const
            {
                auto __front_blocks = __deque->_front_blocks;
                difference_type __block_index_;
                difference_type __elem_index_;
                Area __area_;
                difference_type _current;
                difference_type _after_add;
                // 计算前向块大小(填充后)
                difference_type front_capacity = __front_blocks.size() * block_size;
                if (__area == Area::FRONT)
                {
                     _current = (__front_blocks.size() - __current_block_idx) * block_size - __current_elem_idx;
                     _after_add = _current + n;
                   
                }
                else
                {
                    _current = front_capacity + __current_block_idx * block_size + __current_elem_idx;
                    _after_add = _current + n;
                }
                if (_after_add > front_capacity)
                {
                    _after_add -= front_capacity;
                    __block_index_ = _after_add / block_size;
                    __elem_index_ = _after_add % block_size;
                    __area_ = Area::BACK;
                }
                else
                {
                    __block_index_ = __front_blocks.size() - _after_add / block_size - 1;
                    __elem_index_ = block_size - _after_add % block_size;
                    __area_ = Area::FRONT;
                }
                return iterator_base(this->__deque, __block_index_, __elem_index_, __area_);
            }
            // 减法运算
            iterator_base operator-(difference_type n) const
            {
                return (*this) + (-n);
            }
            
            // 自增
            iterator_base& operator+=(difference_type n)
            {
                auto __front_blocks = __deque->_front_blocks;
                difference_type _current;
                difference_type _after_add;
                // 计算前向块大小(填充后)
                difference_type front_capacity = __front_blocks.size() * block_size;
                if (__area == Area::FRONT)
                {
                    _current = (__front_blocks.size() - __current_block_idx) * block_size - __current_elem_idx;
                    _after_add = _current + n;

                }
                else
                {
                    _current = front_capacity + __current_block_idx * block_size + __current_elem_idx;
                    _after_add = _current + n;
                }
                if (_after_add > front_capacity)
                {
                    _after_add -= front_capacity;
                    __current_block_idx = _after_add / block_size;
                    __current_elem_idx = _after_add % block_size;
                    __area = Area::BACK;
                }
                else
                {
                    __current_block_idx = __front_blocks.size() - _after_add / block_size - 1;
                    __current_elem_idx = block_size - _after_add % block_size;
                    __area = Area::FRONT;
                }
                return *this;
            }
            // 自减
            iterator_base& operator-=(difference_type n)
            {
                (*this) += (-n);
                return *this;
            }

            // 等于
            template <typename OtherU>
                requires (std::is_same_v<OtherU, T> || std::is_same_v<OtherU, const T>)
            bool operator==(const iterator_base<OtherU>& other) const
            {
                assert(__deque == other.__deque);
                return __area == other.__area && __current_block_idx == other.__current_block_idx && __current_elem_idx == other.__current_elem_idx;
            }

            // 三路比较运算符
            template <typename OtherU>
                requires (std::is_same_v<OtherU, T> || std::is_same_v<OtherU, const T>)
            auto operator<=>(const iterator_base<OtherU>& other) const
            {
                assert(__deque == other.__deque);
                if (__area != other.__area)
                {
                    return static_cast<int>(__area) <=> static_cast<int>(other.__area);
                }
                else if (__current_block_idx != other.__current_block_idx)
                {
                    return static_cast<int>(__area) * __current_block_idx <=> tatic_cast<int>(other.__area) * other.__current_block_idx;
                }
                else
                {
                    return static_cast<int>(__area) * __current_block_idx <=> tatic_cast<int>(other.__area) * other.__current_block_idx;
                }

            }
            // 距离运算
            template <typename OtherU>
                requires (std::is_same_v<OtherU, T> || std::is_same_v<OtherU, const T>)
            difference_type operator-(const iterator_base<OtherU>& other) const
            {
                assert(__deque == other.__deque);
                difference_type pos_this;
                difference_type pos_other;
                auto __front_blocks = __deque->_front_blocks;

                // 计算前向块大小(填充后)
                difference_type front_capacity = __front_blocks.size() * block_size;

                if (__area == Area::FRONT)
                {
                    pos_this = (__front_blocks.size() - __current_block_idx) * block_size - __current_elem_idx;
                }
                else
                {
                    pos_this = front_capacity + __current_block_idx * block_size + __current_elem_idx;
                }

                if (other.__area == Area::FRONT)
                {
                    pos_other = (__front_blocks.size() - other.__current_block_idx) * block_size - other.__current_elem_idx;
                }
                else
                {
                    pos_other = front_capacity + other.__current_block_idx * block_size + other.__current_elem_idx;
                }
                return pos_this - pos_other;
            }

        };

        iterator begin() noexcept 
        {
            std::size_t __block_idx_;
            std::size_t __elem_idx_;
            Area __area_ = Area::FRONT;
            if (_front_blocks.empty())
            {
                __block_idx_ = __elem_idx_ = 0;
                __area_ = Area::BACK;
            }
            else if (_start_offset == 0)
            {
                __block_idx_ = _front_blocks.size() - 1;
                __elem_idx_ = block_size - 1;
            }
            else
            {
                __block_idx_ = _front_blocks.size() - 1;
                __elem_idx_ = _start_offset - 1;
            }

            return iterator(this, __block_idx_, __elem_idx_, __area_);
        }
        const_iterator begin() const noexcept
        {
            std::size_t __block_idx_;
            std::size_t __elem_idx_;
            Area __area_ = Area::FRONT;
            if (_front_blocks.empty())
            {
                __block_idx_ = __elem_idx_ = 0;
                __area_ = Area::BACK;
            }
            else if (_start_offset == 0)
            {
                __block_idx_ = _front_blocks.size() - 1;
                __elem_idx_ = block_size - 1;
            }
            else
            {
                __block_idx_ = _front_blocks.size() - 1;
                __elem_idx_ = _start_offset - 1;
            }

            return iterator(this, __block_idx_, __elem_idx_, __area_);
        }
        const_iterator cbegin() const noexcept { return begin(); }
        iterator end() noexcept 
        {
            return iterator(this, _finish_offset == 0 ? _back_blocks.size() : _back_blocks.size() - 1, _finish_offset, Area::BACK);
        }
        const_iterator end() const noexcept
        {
            return iterator(this, _finish_offset == 0 ? _back_blocks.size() : _back_blocks.size() - 1, _finish_offset, Area::BACK);
        }
        const_iterator cend() const noexcept
        {
            return end();
        }
        
    private:
        // 深拷贝单个块
        void __deep_copy_block(block_type* src, block_type* dst)
        {
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                std::memcpy(dst, src, sizeof(block_type));
            }
            else
            {
                for (std::size_t i = 0; i < block_size; ++i)
                {
                    T* src_elem = reinterpret_cast<T*>(&(*src)[i]);
                    T* dst_elem = reinterpret_cast<T*>(&(*dst)[i]);
                    std::allocator_traits<Allocator>::construct(_alloc, dst_elem, *src_elem);
                }
            }
        }



    };
}