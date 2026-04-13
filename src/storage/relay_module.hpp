#ifndef __RELAY_MODULE_HPP
#define __RELAY_MODULE_HPP

#include "posix_module.hpp"
#include "relay_bridge.h"

class relay_module_t : public posix_module_t {
    // Bidirectional: one bridge for sending (flush), one for receiving (restore)
    relay_bridge::RelayBridge send_bridge;
    relay_bridge::RelayBridge recv_bridge;
    bool send_connected = false;
    bool recv_connected = false;

    bool relay_send_file(const std::string &source);
    bool relay_recv_file(const std::string &dest);

public:
    relay_module_t(const std::string &scratch, const std::string &persistent,
                   const std::string &ib_devname,
                   const std::string &send_dpu_ip, uint16_t send_dpu_port,
                   const std::string &recv_dpu_ip, uint16_t recv_dpu_port,
                   const std::string &remote_host_ip, uint16_t remote_host_port);
    virtual ~relay_module_t();
    virtual bool flush(const command_t &cmd);
    virtual bool restore(const command_t &cmd);

    // Direct memory → ring buffer (bypass scratch files)
    virtual bool supports_direct_mem() const override { return send_connected; }
    virtual bool flush_mem(const std::vector<mem_region_t> &regions) override;
    virtual bool restore_mem(std::vector<mem_region_t> &regions) override;
};

#endif //__RELAY_MODULE_HPP
