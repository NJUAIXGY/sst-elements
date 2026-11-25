// SnnDL V5 Storage-M4: bounded fabric ingress credit protocol.
//
// MemFlowCredit is the single sideband message exchanged between a
// credit-aware sender port (CreditMemLinkV5) and a bounded memory fabric
// ingress (SnnDL.MemoryFabricV5).  It carries no memory command, address,
// or transaction identity — only capacity bookkeeping units for the
// point-to-point credit link it travels on.
//
// The accounting unit is one fixed Message Slot: a complete store-and-forward
// message occupies exactly one ingress entry independent of its payload size.
// Keeping the unit here prevents sender and fabric from drifting into different
// capacity interpretations.  Payload bytes still determine serialization time
// on the fabric lane; they do not subdivide or multiply buffer occupancy.

#ifndef _MEMHIERARCHY_MEMFLOWCREDIT_H_
#define _MEMHIERARCHY_MEMFLOWCREDIT_H_

#include <cstdint>

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

/* One complete message reserves one Message Slot. */
inline constexpr uint64_t memFlowMessageSlots() { return 1; }

} //namespace MemHierarchy
} //namespace SST

#endif
