#pragma once

#include <utility>

//! Runs its function when it goes out of scope, however the scope is left.
template <class TFunction> class ScopeExit
{
public:
    explicit ScopeExit(TFunction aFunction) noexcept
        : m_function(std::move(aFunction))
    {
    }

    ~ScopeExit() noexcept { m_function(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    TFunction m_function;
};
