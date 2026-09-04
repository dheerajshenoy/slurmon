#pragma once

#include "pch.hpp"

// SLURM job struct

class Job
{
public:
    Job(std::string id, std::string name, std::string state, std::string user,
        std::string time, std::string nodes, std::string nodelist_or_reason)
        : m_id(id), m_name(name), m_state(state), m_user(user), m_time(time),
          m_nodes(nodes), m_nodelist_or_reason(nodelist_or_reason)
    {
    }

    inline std::string id() const
    {
        return m_id;
    }

    inline std::string name() const
    {
        return m_name;
    }

    inline std::string state() const
    {
        return m_state;
    }

    inline std::string user() const
    {
        return m_user;
    }

    inline std::string time() const
    {
        return m_time;
    }

    inline std::string nodes() const
    {
        return m_nodes;
    }

    inline std::string nodelist_or_reason() const
    {
        return m_nodelist_or_reason;
    }

private:
    std::string m_id;
    std::string m_name;
    std::string m_state;
    std::string m_user;
    std::string m_time;
    std::string m_nodes;
    std::string m_nodelist_or_reason;
};
