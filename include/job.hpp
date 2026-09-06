#pragma once

#include "pch.hpp"

#include <string>
#include <unordered_map>

// SLURM job — a bag of pipe-delimited fields keyed by our column name.
// Legacy accessors below wrap get() for the columns the UI hardcodes
// (details/logs/cancel dialogs).
class Job
{
public:
    Job() = default;

    void set(const std::string &key, std::string value)
    {
        m_fields[key] = std::move(value);
    }

    const std::string &get(const std::string &key) const
    {
        auto it = m_fields.find(key);
        if (it == m_fields.end())
        {
            static const std::string empty;
            return empty;
        }
        return it->second;
    }

    const std::string &id() const                 { return get("id"); }
    const std::string &name() const               { return get("name"); }
    const std::string &state() const              { return get("state"); }
    const std::string &user() const               { return get("user"); }
    const std::string &time() const               { return get("time"); }
    const std::string &nodes() const              { return get("nodes"); }
    const std::string &nodelist_or_reason() const { return get("nodelist"); }

private:
    std::unordered_map<std::string, std::string> m_fields;
};
