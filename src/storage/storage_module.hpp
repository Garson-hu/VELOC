#ifndef __STORAGE_MODULE_HPP
#define __STORAGE_MODULE_HPP

#include <set>
#include <vector>
#include <utility>
#include "common/command.hpp"

// A memory region: (region_id, pointer, size)
using mem_region_t = std::pair<int, std::pair<void*, size_t>>;

class storage_module_t {
public:
    storage_module_t(...);
    virtual void get_versions(const command_t &cmd, std::set<int> &result);
    virtual bool remove(const command_t &cmd);
    virtual bool flush(const command_t &cmd);
    virtual bool restore(const command_t &cmd);
    virtual bool exists(const command_t &cmd);

    // Direct memory transfer (bypass scratch files).
    // Default: not supported. Override in relay_module_t.
    virtual bool supports_direct_mem() const { return false; }
    virtual bool flush_mem(const std::vector<mem_region_t> &regions) { return false; }
    virtual bool restore_mem(std::vector<mem_region_t> &regions) { return false; }

    virtual ~storage_module_t();
};

#endif //__STORAGE_MODULE_HPP
