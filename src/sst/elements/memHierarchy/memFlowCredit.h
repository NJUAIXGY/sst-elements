// SnnDL V5 Storage-M4: bounded fabric ingress credit protocol.
//
// MemFlowCredit is the single sideband message exchanged between a
// credit-aware sender port (CreditMemLinkV5) and a bounded memory fabric
// ingress (SnnDL.MemoryFabricV5).  It carries no memory command, address,
// or transaction identity — only capacity bookkeeping units for the
// point-to-point credit link it travels on.
//
// memFlowCreditsForPayload is the ONE buffer-occupancy formula shared by
// the sender (trySend) and the fabric (receive accounting): keeping it in
// this shared header prevents the two sides from drifting into different
// rounding rules (design section 17).

#ifndef _MEMHIERARCHY_MEMFLOWCREDIT_H_
#define _MEMHIERARCHY_MEMFLOWCREDIT_H_

#include <cstdint>
#include <algorithm>

#include <sst/core/event.h>
#include <sst/core/serialization/serialize.h>

namespace SST {
namespace MemHierarchy {

enum class MemFlowCreditKind : uint8_t {
    InitialGrant,   // fabric -> sender, untimed init phase only
    ReturnCredit,   // fabric -> sender, timed phase only
};

class MemFlowCredit : public SST::Event {
public:
    MemFlowCreditKind kind = MemFlowCreditKind::ReturnCredit;
    uint64_t          units = 0;

    MemFlowCredit() = default;
    MemFlowCredit(MemFlowCreditKind k, uint64_t u) : kind(k), units(u) { }

    MemFlowCredit* clone() override {
        return new MemFlowCredit(*this);
    }

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        SST_SER(kind);
        SST_SER(units);
    }

private:
    ImplementSerializable(SST::MemHierarchy::MemFlowCredit)
};

/* Buffer-space credits a whole message reserves:
 *   K(e) = max(1, ceil((descriptor_bytes + getPayloadSize()) / granularity))
 * Store-and-forward admission — never half a message. */
inline uint64_t memFlowCreditsForPayload(uint64_t descriptorBytes,
                                         uint64_t payloadBytes,
                                         uint64_t granularityBytes) {
    const uint64_t total = descriptorBytes + payloadBytes;
    return std::max<uint64_t>(1, (total + granularityBytes - 1) / granularityBytes);
}

} //namespace MemHierarchy
} //namespace SST

#endif
