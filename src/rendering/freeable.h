#pragma once

class IFreeable
{
public:
    virtual ~IFreeable() = default;

    virtual void free() = 0;
};
