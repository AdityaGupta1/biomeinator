/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
