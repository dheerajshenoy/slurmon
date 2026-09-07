#pragma once

#include "pch.hpp"

#include <string>
#include <string_view>
#include <vector>

// Column index lookup — resolved once against the static column table
// defined in slurmon.cpp. Returns -1 for unknown keys.
int column_index(std::string_view key);
size_t column_count();

// SLURM job — one string per column, indexed by column_index().
class Job
{
public:
    Job() : m_fields(column_count()) {}

    void set(int idx, std::string value)
    {
        if (idx < 0)
            return;
        if (static_cast<size_t>(idx) >= m_fields.size())
            m_fields.resize(idx + 1);
        m_fields[idx] = std::move(value);
    }

    const std::string &get(int idx) const
    {
        static const std::string empty;
        if (idx < 0 || static_cast<size_t>(idx) >= m_fields.size())
            return empty;
        return m_fields[idx];
    }

    void set(std::string_view key, std::string value)
    {
        set(column_index(key), std::move(value));
    }

    const std::string &get(std::string_view key) const
    {
        return get(column_index(key));
    }

    const std::string &id() const;
    const std::string &name() const;
    const std::string &state() const;
    const std::string &user() const;
    const std::string &time() const;
    const std::string &nodes() const;
    const std::string &nodelist_or_reason() const;

private:
    std::vector<std::string> m_fields;
};
