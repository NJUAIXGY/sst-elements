// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#include <sst_config.h>
#include "sst/elements/memHierarchy/util.h"
#include "membackend/ramulator2Backend.h"
#include "base/config.h"

#include <cinttypes>

using namespace SST;
using namespace SST::MemHierarchy;


ramulator2Memory::ramulator2Memory(ComponentId_t id, Params &params) :
    SimpleMemBackend(id, params)
{
    config_path = params.find<std::string>("configFile",
                                            NO_STRING_DEFINED);
    if (config_path == NO_STRING_DEFINED) {
        output->fatal(CALL_INFO, -1, "Ramulator2 Backend must define a 'configFile' file parameter\n");
    }

    YAML::Node config = Ramulator::Config::parse_config_file(config_path, {});
    ramulator2_frontend = Ramulator::Factory::create_frontend(config);
    ramulator2_memorysystem = Ramulator::Factory::create_memory_system(config);

    ramulator2_frontend->connect_memory_system(ramulator2_memorysystem);
    ramulator2_memorysystem->connect_frontend(ramulator2_frontend);

    int32_t admission_queue_size = params.find<int32_t>("admission_queue_size", 0);
    admission_queue_size_ = static_cast<int64_t>(admission_queue_size);
    if (admission_queue_size_ < 0) admission_queue_size_ = -1;
    admission_issue_budget_per_cycle_ = params.find<int32_t>("admission_issue_budget_per_cycle", -1);
    if (admission_issue_budget_per_cycle_ == 0) admission_issue_budget_per_cycle_ = -1;
    memory_ticks_per_sst_cycle_ = params.find<int32_t>("memory_ticks_per_sst_cycle", 1);
    sst_cycles_per_memory_tick_ = params.find<int32_t>("sst_cycles_per_memory_tick", 1);
    if (memory_ticks_per_sst_cycle_ < 1 || sst_cycles_per_memory_tick_ < 1) {
        output->fatal(CALL_INFO, -1,
            "Ramulator2 clock controls must be >= 1 (memory_ticks_per_sst_cycle=%d, sst_cycles_per_memory_tick=%d)\n",
            memory_ticks_per_sst_cycle_, sst_cycles_per_memory_tick_);
    }
    if (memory_ticks_per_sst_cycle_ > 1 && sst_cycles_per_memory_tick_ > 1) {
        output->fatal(CALL_INFO, -1,
            "Ramulator2 clock controls cannot both be greater than one\n");
    }
    sst_cycle_phase_ = 0;
    admission_queue_enable_ = (admission_queue_size_ != 0);
    trace_file_ = params.find<std::string>("trace_file", "");
    if (!trace_file_.empty()) {
        trace_stream_.open(trace_file_, std::ios::out | std::ios::trunc);
        if (!trace_stream_.good()) output->fatal(CALL_INFO, -1, "cannot open Ramulator2 trace file %s\n", trace_file_.c_str());
    }

    output->output(CALL_INFO, "Instantiated Ramulator2 from config file %s\n", config_path);
    output->verbose(CALL_INFO, 1, 0,
        "[ramu2] admission_queue enable=%d size=%" PRId64 " issue_budget_per_cycle=%" PRId32 "\n",
        admission_queue_enable_ ? 1 : 0, admission_queue_size_, admission_issue_budget_per_cycle_);
    output->verbose(CALL_INFO, 1, 0,
        "[ramu2] clock ratio memory_ticks_per_sst_cycle=%" PRId32 " sst_cycles_per_memory_tick=%" PRId32 "\n",
        memory_ticks_per_sst_cycle_, sst_cycles_per_memory_tick_);
}

bool ramulator2Memory::issueToRamulator_(const PendingReq& req){
    traceEvent_("submit", req.reqId, req.addr, req.isWrite, false);
    output->verbose(CALL_INFO, 1, 0,
        "[ramu2] issueRequest id=%" PRIu64 " addr=0x%" PRIx64 " isWrite=%d size=%u\n",
        (uint64_t)req.reqId, (uint64_t)req.addr, (int)req.isWrite, req.numBytes);

    bool enqueue_success = false;

    if (req.isWrite) {
        enqueue_success = ramulator2_frontend->receive_external_requests(1, req.addr, 0,
            [this](Ramulator::Request& req) {});
        if (enqueue_success) {
            writes.insert(req.reqId);
            write_addrs[req.reqId] = req.addr;
        }
    } else {
        enqueue_success = ramulator2_frontend->receive_external_requests(0, req.addr, 0,
            [this](Ramulator::Request& req) {
                output->verbose(CALL_INFO, 1, 0,
                    "[ramu2] Read callback addr=0x%" PRIx64 " outstanding=%zu\n",
                    (uint64_t)req.addr, dramReqs.count(req.addr) ? dramReqs.at(req.addr).size() : 0);
                std::deque<ReqId> &reqs = dramReqs[req.addr];

                if (reqs.empty())
                    output->fatal(CALL_INFO, -1, "Ramulator2Backend: Error - ramulator2Done called but dramReqs[addr] is empty. Addr: %" PRIx64 "\n", (Addr)req.addr);

                ReqId memreq = reqs.front();
                reqs.pop_front();
                if(0 == reqs.size())
                    dramReqs.erase(req.addr);

                traceEvent_("complete", memreq, req.addr, false, true, &req);
                handleMemResponse(memreq);
        });
        if (enqueue_success) {
            if (dramReqs.find(req.addr) != dramReqs.end()) dramReqs[req.addr].push_back(req.reqId);
            else {
                std::deque<ReqId> reqs;
                reqs.push_back(req.reqId);
                dramReqs.insert(std::make_pair(req.addr,reqs));
            }
        }
    }
    output->verbose(CALL_INFO, 1, 0,
        "[ramu2] backend enqueue %s (dramReqMap=%zu writes=%zu admissionQ=%zu)\n",
        enqueue_success ? "successful" : "unsuccessful", dramReqs.size(), writes.size(), admission_queue_.size());

    traceEvent_("admit", req.reqId, req.addr, req.isWrite, enqueue_success);
    return enqueue_success;
}

bool ramulator2Memory::issueRequest(ReqId reqId, Addr addr, bool isWrite, unsigned numBytes){
    PendingReq req{reqId, addr, isWrite, numBytes};
    if (!admission_queue_enable_) {
        return issueToRamulator_(req);
    }

    if (admission_queue_size_ >= 0 && admission_queue_.size() >= static_cast<size_t>(admission_queue_size_)) {
        output->verbose(CALL_INFO, 1, 0,
            "[ramu2] admission queue full, reject id=%" PRIu64 " size=%zu cap=%" PRId64 "\n",
            (uint64_t)reqId, admission_queue_.size(), admission_queue_size_);
        return false;
    }

    admission_queue_.push_back(req);
    output->verbose(CALL_INFO, 2, 0,
        "[ramu2] admission enqueue ok id=%" PRIu64 " qsize=%zu\n",
        (uint64_t)reqId, admission_queue_.size());
    return true;
}

bool ramulator2Memory::clock(Cycle_t cycle){
    trace_cycle_ = static_cast<std::uint64_t>(cycle);
    output->verbose(CALL_INFO, 2, 0,
        "[ramu2] clock cycle=%" PRIu64 " pending_reads=%zu pending_writes=%zu admission_q=%zu\n",
        (uint64_t)cycle, dramReqs.size(), writes.size(), admission_queue_.size());

    int issued_this_cycle = 0;
    while (admission_queue_enable_ && !admission_queue_.empty()) {
        if (admission_issue_budget_per_cycle_ > 0 &&
            issued_this_cycle >= admission_issue_budget_per_cycle_) {
            break;
        }
        const PendingReq& req = admission_queue_.front();
        if (!issueToRamulator_(req)) {
            break;
        }
        admission_queue_.pop_front();
        issued_this_cycle++;
    }

    bool tick_memory = true;
    if (sst_cycles_per_memory_tick_ > 1) {
        tick_memory = (sst_cycle_phase_ == 0);
        sst_cycle_phase_ = (sst_cycle_phase_ + 1) % sst_cycles_per_memory_tick_;
    }
    if (tick_memory) {
        for (int32_t tick = 0; tick < memory_ticks_per_sst_cycle_; ++tick) {
            ramulator2_frontend->tick();
        }
    }
    // Ack writes since ramulator won't
    while (!writes.empty()) {
        const auto id = *writes.begin();
        const auto address_it = write_addrs.find(id);
        const Addr address = address_it == write_addrs.end() ? 0 : address_it->second;
        traceEvent_("complete", id, address, true, true);
        handleMemResponse(id);
        writes.erase(writes.begin());
        if (address_it != write_addrs.end()) write_addrs.erase(address_it);
    }
    return false;
}

void ramulator2Memory::finish(){
    if (trace_stream_.is_open()) trace_stream_.flush();
    ramulator2_frontend->finalize();
    ramulator2_memorysystem->finalize();
}

void ramulator2Memory::traceEvent_(const char* phase, ReqId id, Addr addr, bool isWrite,
                                   bool accepted, const Ramulator::Request* request) {
    if (!trace_stream_.good()) return;
    trace_stream_ << "{\"schema_version\":\"snndl-ramulator2-request-event/v1\""
                  << ",\"phase\":\"" << phase << "\",\"request_id\":" << id
                  << ",\"address\":" << addr << ",\"is_write\":" << (isWrite ? "true" : "false")
                  << ",\"accepted\":" << (accepted ? "true" : "false")
                  << ",\"cycle\":" << trace_cycle_;
    if (request) {
        trace_stream_ << ",\"channel\":\"unavailable\",\"bank\":\"unavailable\",\"row\":\"unavailable\"";
    }
    trace_stream_ << "}\n";
}
