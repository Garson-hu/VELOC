#ifndef __RELAY_MODULE_HPP
#define __RELAY_MODULE_HPP

#include "posix_module.hpp"
#include "relay_bridge.h"

#include <map>

class relay_module_t : public posix_module_t {
    // Bidirectional: one bridge for sending (flush), one for receiving (restore)
    relay_bridge::RelayBridge send_bridge;
    relay_bridge::RelayBridge recv_bridge;
    bool send_connected = false;
    bool recv_connected = false;
    bool async_mode = false;
    bool use_register_once = true;
    // When use_register_once && use_adapter_staging, multi-region apps copy
    // every region into one big pinned staging buffer and issue a single
    // bridge.transfer over that buffer. Single-region apps see no change.
    // The cost is one host-side memcpy per region; the gain is that the DPU
    // sees ONE region instead of N, so the per-region MMO setup cost (which
    // transfer_batch could not amortise away) collapses to a single setup.
    bool use_adapter_staging = false;

    // Register-once cache keyed by VELOC region id. On first flush_mem for an
    // id we pay create_cgmk_mkey + NEW_REGION_DESC; subsequent checkpoints hit
    // the hot path with zero mkey work. If the app re-protects the id with a
    // different (ptr, size), we retire the old alias and re-register.
    struct region_cache_entry_t {
        relay_bridge::RegionHandle handle;
        void  *ptr;
        size_t size;
    };
    std::map<int, region_cache_entry_t> region_cache;

    // Adapter staging buffer for the unified register-once path. Allocated
    // lazily on the first flush_mem call that needs it; grown (and re-
    // registered with the bridge) when a later checkpoint exceeds the
    // current capacity. NULL until first use.
    void                       *adapter_staging_buf  = nullptr;
    size_t                      adapter_staging_size = 0;
    relay_bridge::RegionHandle  adapter_staging_handle{};
    bool                        adapter_staging_registered = false;

    bool relay_send_file(const std::string &source);
    bool relay_recv_file(const std::string &dest);

    bool ensure_region_registered(int id, void *ptr, size_t size,
                                  relay_bridge::RegionHandle &out);
    void unregister_all_regions();

    // Resize the adapter staging buffer to at least `min_bytes` and re-
    // register it with the bridge. Returns true on success.
    bool ensure_adapter_staging(size_t min_bytes);

public:
    relay_module_t(const std::string &scratch, const std::string &persistent,
                   const std::string &ib_devname,
                   const std::string &send_dpu_ip, uint16_t send_dpu_port,
                   const std::string &recv_dpu_ip, uint16_t recv_dpu_port,
                   const std::string &remote_host_ip, uint16_t remote_host_port,
                   bool async_mode = false,
                   bool use_register_once = true,
                   bool use_adapter_staging = false);
    virtual ~relay_module_t();
    virtual bool flush(const command_t &cmd);
    virtual bool restore(const command_t &cmd);

    // Direct memory → ring buffer (bypass scratch files)
    virtual bool supports_direct_mem() const override { return send_connected; }
    virtual bool flush_mem(const std::vector<mem_region_t> &regions) override;
    virtual bool restore_mem(std::vector<mem_region_t> &regions) override;
};

#endif //__RELAY_MODULE_HPP
