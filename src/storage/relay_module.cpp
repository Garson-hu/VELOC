#include "relay_module.hpp"
#include "common/file_util.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <algorithm>

//#define __DEBUG
#include "common/debug.hpp"

relay_module_t::relay_module_t(const std::string &s, const std::string &p,
                               const std::string &ib_devname,
                               const std::string &send_dpu_ip, uint16_t send_dpu_port,
                               const std::string &recv_dpu_ip, uint16_t recv_dpu_port,
                               const std::string &remote_host_ip, uint16_t remote_host_port,
                               bool async_mode)
    : posix_module_t(s, p), async_mode(async_mode)
{
    // --- sender bridge (this host -> remote host) ---
    relay_bridge::Config send_cfg;
    send_cfg.ib_devname       = ib_devname;
    send_cfg.dpu_ip           = send_dpu_ip;
    send_cfg.dpu_port         = send_dpu_port;
    send_cfg.role             = relay_bridge::Role::SENDER;
    send_cfg.remote_host_ip   = remote_host_ip;
    send_cfg.remote_host_port = remote_host_port;

    auto status = send_bridge.connect(send_cfg);
    if (status != relay_bridge::Status::OK)
        FATAL("RelayBridge sender connect failed: " << relay_bridge::status_string(status));
    send_connected = true;
    INFO("RelayBridge sender connected (DPU: " << send_dpu_ip << ":" << send_dpu_port << ")");

    // --- receiver bridge (remote host -> this host) ---
    // Skip if recv_dpu_ip is empty (send-only mode)
    if (!recv_dpu_ip.empty()) {
        relay_bridge::Config recv_cfg;
        recv_cfg.ib_devname       = ib_devname;
        recv_cfg.dpu_ip           = recv_dpu_ip;
        recv_cfg.dpu_port         = recv_dpu_port;
        recv_cfg.role             = relay_bridge::Role::RECEIVER;
        recv_cfg.remote_host_ip   = remote_host_ip;
        recv_cfg.remote_host_port = remote_host_port;

        status = recv_bridge.connect(recv_cfg);
        if (status != relay_bridge::Status::OK)
            FATAL("RelayBridge receiver connect failed: " << relay_bridge::status_string(status));
        recv_connected = true;
        INFO("RelayBridge receiver connected (DPU: " << recv_dpu_ip << ":" << recv_dpu_port << ")");
    }
}

relay_module_t::~relay_module_t() {
    if (send_connected) {
        send_bridge.disconnect();
        send_connected = false;
    }
    if (recv_connected) {
        recv_bridge.disconnect();
        recv_connected = false;
    }
}

bool relay_module_t::relay_send_file(const std::string &source) {
    TIMER_START(io_timer);

    ssize_t fsize = file_size(source);
    if (fsize < 0) {
        ERROR("cannot stat " << source << ", error = " << std::strerror(errno));
        return false;
    }
    if (fsize == 0) {
        DBG("skipping empty file " << source);
        return true;
    }

    size_t max_chunk = send_bridge.max_transfer_size();
    std::vector<unsigned char> buf(std::min((size_t)fsize, max_chunk));

    int fd = open(source.c_str(), O_RDONLY);
    if (fd == -1) {
        ERROR("cannot open " << source << ", error = " << std::strerror(errno));
        return false;
    }

    size_t remaining = (size_t)fsize;
    bool success = true;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, max_chunk);
        ssize_t bytes_read = read(fd, buf.data(), chunk);
        if (bytes_read != (ssize_t)chunk) {
            ERROR("read error on " << source << ", error = " << std::strerror(errno));
            success = false;
            break;
        }
        auto status = send_bridge.send(buf.data(), chunk);
        if (status != relay_bridge::Status::OK) {
            ERROR("RelayBridge send failed: " << relay_bridge::status_string(status));
            success = false;
            break;
        }
        remaining -= chunk;
    }
    close(fd);

    if (success) {
        TIMER_STOP(io_timer, "relay-sent " << source << " (" << fsize << " bytes)");
    }
    return success;
}

bool relay_module_t::relay_recv_file(const std::string &dest) {
    TIMER_START(io_timer);

    size_t max_chunk = recv_bridge.max_transfer_size();
    std::vector<unsigned char> buf(max_chunk);

    int fd = open(dest.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        ERROR("cannot open " << dest << ", error = " << std::strerror(errno));
        return false;
    }

    size_t received = 0;
    auto status = recv_bridge.recv(buf.data(), max_chunk, &received);
    if (status != relay_bridge::Status::OK) {
        ERROR("RelayBridge recv failed: " << relay_bridge::status_string(status));
        close(fd);
        return false;
    }

    if (received > 0) {
        ssize_t written = write(fd, buf.data(), received);
        if (written != (ssize_t)received) {
            ERROR("write error on " << dest << ", error = " << std::strerror(errno));
            close(fd);
            return false;
        }
    }
    close(fd);

    TIMER_STOP(io_timer, "relay-received " << dest << " (" << received << " bytes)");
    return true;
}

bool relay_module_t::flush(const command_t &cmd) {
    // memory-based API: checkpoint is in scratch, relay it out
    if (cmd.original[0] == 0)
        return relay_send_file(cmd.filename(scratch));
    // file-based API: relay the routed file
    return relay_send_file(cmd.original);
}

bool relay_module_t::restore(const command_t &cmd) {
    // Receive from relay into the scratch file
    return relay_recv_file(cmd.filename(scratch));
}

// ── Direct memory transfer (no scratch files) ──────────────────────
//
// Header format written into the first slot(s):
//   [num_regions (size_t)] [id0 (int) | size0 (size_t)] [id1 | size1] ...
// Then each region's data follows in subsequent slot(s).
//

bool relay_module_t::flush_mem(const std::vector<mem_region_t> &regions) {
    TIMER_START(io_timer);

    if (!send_connected) {
        ERROR("relay send bridge not connected");
        return false;
    }

    size_t max_chunk = send_bridge.max_transfer_size();

    // Build header: [num_regions | id0, size0 | id1, size1 | ...]
    size_t num_regions = regions.size();
    size_t header_size = sizeof(size_t) + num_regions * (sizeof(int) + sizeof(size_t));
    std::vector<unsigned char> header(header_size);
    unsigned char *hp = header.data();
    memcpy(hp, &num_regions, sizeof(size_t));
    hp += sizeof(size_t);
    for (auto &r : regions) {
        int id = r.first;
        size_t sz = r.second.second;
        memcpy(hp, &id, sizeof(int));    hp += sizeof(int);
        memcpy(hp, &sz, sizeof(size_t)); hp += sizeof(size_t);
    }

    // Send header via convenience API (small, one slot is enough)
    auto status = send_bridge.send(header.data(), header_size);
    if (status != relay_bridge::Status::OK) {
        ERROR("RelayBridge send header failed: " << relay_bridge::status_string(status));
        return false;
    }

    // Send each region's data directly from application memory
    size_t total_bytes = 0;
    for (auto &r : regions) {
        void *ptr   = r.second.first;
        size_t size = r.second.second;
        if (ptr == NULL || size == 0)
            continue;

        // Chunk large regions across multiple slots
        unsigned char *src = (unsigned char *)ptr;
        size_t remaining = size;
        while (remaining > 0) {
            size_t chunk = std::min(remaining, max_chunk);

            // Zero-copy: write directly from app memory into ring buffer slot
            size_t capacity = 0;
            void *slot = send_bridge.get_write_slot(&capacity);
            if (!slot && async_mode) {
                // Ring full — flush in-flight slots and retry
                send_bridge.flush();
                slot = send_bridge.get_write_slot(&capacity);
            }
            if (!slot) {
                ERROR("relay ring buffer full");
                return false;
            }
            memcpy(slot, src, chunk);
            if (async_mode) {
                auto s = send_bridge.commit_slot_async(chunk);
                if (s != relay_bridge::Status::OK) {
                    ERROR("RelayBridge commit_slot_async failed: " << relay_bridge::status_string(s));
                    return false;
                }
            } else {
                auto s = send_bridge.commit_slot(chunk);
                if (s != relay_bridge::Status::OK) {
                    ERROR("RelayBridge commit_slot failed: " << relay_bridge::status_string(s));
                    return false;
                }
            }

            src += chunk;
            remaining -= chunk;
        }
        total_bytes += size;
    }

    // In async mode, return immediately after submitting all slots. The relay's
    // background drain thread collects completions; the next flush_mem call
    // auto-backpressures via get_write_slot when the ring is full. This lets
    // the caller overlap compute with RDMA transfer.

    TIMER_STOP(io_timer, "relay-flush-mem " << regions.size()
               << " regions (" << total_bytes << " bytes)");
    return true;
}

bool relay_module_t::restore_mem(std::vector<mem_region_t> &regions) {
    TIMER_START(io_timer);

    if (!recv_connected) {
        ERROR("relay recv bridge not connected");
        return false;
    }

    // Receive header
    size_t max_chunk = recv_bridge.max_transfer_size();
    std::vector<unsigned char> header_buf(max_chunk);
    size_t received = 0;
    auto status = recv_bridge.recv(header_buf.data(), max_chunk, &received);
    if (status != relay_bridge::Status::OK) {
        ERROR("RelayBridge recv header failed: " << relay_bridge::status_string(status));
        return false;
    }

    // Parse header
    unsigned char *hp = header_buf.data();
    size_t num_regions = 0;
    memcpy(&num_regions, hp, sizeof(size_t));
    hp += sizeof(size_t);

    // Build map of id -> (size) from header
    struct region_meta { int id; size_t size; };
    std::vector<region_meta> meta(num_regions);
    for (size_t i = 0; i < num_regions; i++) {
        memcpy(&meta[i].id, hp, sizeof(int));    hp += sizeof(int);
        memcpy(&meta[i].size, hp, sizeof(size_t)); hp += sizeof(size_t);
    }

    // Build lookup from regions vector (id -> ptr, size)
    std::map<int, std::pair<void*, size_t>> region_map;
    for (auto &r : regions)
        region_map[r.first] = r.second;

    // Receive each region's data directly into application memory
    size_t total_bytes = 0;
    for (auto &m : meta) {
        auto it = region_map.find(m.id);
        if (it == region_map.end()) {
            ERROR("no protected memory region for id " << m.id);
            return false;
        }
        void *ptr = it->second.first;
        size_t buf_size = it->second.second;
        if (buf_size < m.size) {
            ERROR("protected memory region " << m.id << " too small ("
                  << buf_size << ") for " << m.size << " bytes");
            return false;
        }

        // Receive chunks directly into application memory
        unsigned char *dst = (unsigned char *)ptr;
        size_t remaining = m.size;
        while (remaining > 0) {
            size_t data_size = 0;
            const void *slot = recv_bridge.poll_read_slot(&data_size);
            if (!slot) {
                ERROR("RelayBridge poll_read_slot failed");
                return false;
            }
            memcpy(dst, slot, data_size);
            recv_bridge.release_slot();

            dst += data_size;
            remaining -= data_size;
        }
        total_bytes += m.size;
    }

    TIMER_STOP(io_timer, "relay-restore-mem " << num_regions
               << " regions (" << total_bytes << " bytes)");
    return true;
}
