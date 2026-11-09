// mvp_tlm.h - put a SystemC/TLM-2.0 model behind minisim's AXI4-Lite bridge.
//
// A TLM model already has the shape mvp needs: an address, a length, data and
// a command. This header is the adapter, so a subsystem that exists as TLM
// (a CPU model, a DMA engine, a vendor's reference model) answers the design
// without being rewritten.
//
//   #include <systemc>
//   #include "mvp_tlm.h"
//
//   my_tlm_device dev("dev");                  // any tlm_fw_transport_if target
//   static mvp::tlm_bridge bridge("ram", 0x0, 0x10000);
//
//   MVP_MODEL(init) {
//       bridge.socket.bind(dev.socket);        // the usual TLM binding
//       bridge.add();                          // put it in minisim's map
//   }
//
// Build it with SystemC on the command line, because msim builds the model
// files for you but does not know where SystemC is:
//
//   msim -mvp soc_tb.sv model.cpp -top soc_tb \
//        -CFLAGS "-I$SYSTEMC_HOME/include -std=c++17" \
//        -LDFLAGS "-L$SYSTEMC_HOME/lib -lsystemc"
//
// Time: the design's clock drives SystemC, it is not the other way round.
// mvp::tlm_bridge runs sc_start for the time that passed since the last access
// (b_transport's own delay included), so a TLM model that waits sees time move
// while the design runs. A model that needs to run on its own between accesses
// should also arm mvp_after_ps and call mvp::tlm_advance() from the callback.
//
// This header needs SystemC; without it, nothing else in mvp does.
#ifndef MVP_TLM_H
#define MVP_TLM_H

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

#include <cstring>
#include <string>

#include "mvp.h"

namespace mvp {

// Run SystemC up to the design's time. Safe to call at any point.
inline void tlm_advance() {
    sc_core::sc_time now = sc_core::sc_time((double)mvp_time_ps(), sc_core::SC_PS);
    sc_core::sc_time ran = sc_core::sc_time_stamp();
    if (now > ran) sc_core::sc_start(now - ran);
}

// One TLM target in minisim's memory map.
class tlm_bridge : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<tlm_bridge> socket;

    tlm_bridge(const char* name, uint64_t base, uint64_t size)
        : sc_core::sc_module(sc_core::sc_module_name(name)),
          socket("socket"),
          name_(name),
          base_(base),
          size_(size) {}

    // Register with minisim. Call it after the socket is bound.
    int add() {
        mvp_device dev;
        std::memset(&dev, 0, sizeof dev);
        dev.name = name_.c_str();
        dev.base = base_;
        dev.size = size_;
        dev.ctx = this;
        dev.read = &tlm_bridge::read_thunk;
        dev.write = &tlm_bridge::write_thunk;
        return mvp_add_device(&dev);
    }

private:
    static int read_thunk(void* ctx, uint64_t addr, uint32_t bytes, uint64_t* data) {
        return static_cast<tlm_bridge*>(ctx)->access(addr, bytes, data, nullptr, 0);
    }
    static int write_thunk(void* ctx, uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb) {
        uint64_t d = data;
        return static_cast<tlm_bridge*>(ctx)->access(addr, bytes, &d, &d, strb);
    }

    // One b_transport, with the design's time brought to SystemC first and the
    // transaction's own delay added after.
    int access(uint64_t addr, uint32_t bytes, uint64_t* data, uint64_t* wdata, uint8_t strb) {
        tlm_advance();

        tlm::tlm_generic_payload tr;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        uint8_t buf[8];
        uint8_t be[8];

        if (wdata) {
            for (uint32_t i = 0; i < bytes; i++) buf[i] = (uint8_t)(*wdata >> (8 * i));
        } else {
            std::memset(buf, 0, sizeof buf);
        }
        tr.set_command(wdata ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tr.set_address(addr);
        tr.set_data_ptr(buf);
        tr.set_data_length(bytes);
        tr.set_streaming_width(bytes);
        tr.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        // Byte enables, for a partial write (TLM 2.0, 14.9).
        bool partial = wdata && strb != (uint8_t)((bytes == 8) ? 0xff : ((1u << bytes) - 1));
        if (partial) {
            for (uint32_t i = 0; i < bytes; i++) be[i] = (strb >> i) & 1u ? 0xff : 0x00;
            tr.set_byte_enable_ptr(be);
            tr.set_byte_enable_length(bytes);
        } else {
            tr.set_byte_enable_ptr(nullptr);
            tr.set_byte_enable_length(0);
        }

        socket->b_transport(tr, delay);
        if (delay > sc_core::SC_ZERO_TIME) sc_core::sc_start(delay);

        if (tr.is_response_error()) return MVP_ERR;
        if (!wdata) {
            uint64_t v = 0;
            for (uint32_t i = 0; i < bytes; i++) v |= (uint64_t)buf[i] << (8 * i);
            *data = v;
        }
        return MVP_OK;
    }

    std::string name_;
    uint64_t base_, size_;
};

}  // namespace mvp

#endif  // MVP_TLM_H
