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

    std::string get(const std::string &key) const
    {
        auto it = m_fields.find(key);
        return it == m_fields.end() ? std::string() : it->second;
    }

    std::string id() const                 { return get("id"); }
    std::string name() const               { return get("name"); }
    std::string state() const              { return get("state"); }
    std::string user() const               { return get("user"); }
    std::string time() const               { return get("time"); }
    std::string nodes() const              { return get("nodes"); }
    std::string nodelist_or_reason() const { return get("nodelist"); }

private:
    std::unordered_map<std::string, std::string> m_fields;
};
