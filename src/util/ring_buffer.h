// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <array>
#include <cassert>
#include <cstddef>

template<typename T, size_t N>
class RingBuffer
{
    static_assert(N > 0);

private:
    std::array<T, N> buffer{};
    size_t offset;
    size_t size;

public:
    void push(const T& value)
    {
        this->buffer[this->offset] = value;
        this->offset = (this->offset + 1) % N;
        if (this->size < N)
        {
            ++this->size;
        }
    }

    const std::array<T, N>& getData() const
    {
        return this->buffer;
    }

    size_t getSize() const
    {
        return this->size;
    }

    size_t getOffset() const
    {
        return this->offset;
    }

    constexpr size_t getMaxSize() const
    {
        return N;
    }

    void clear()
    {
        this->size = 0;
        this->offset = 0;
    }
};
