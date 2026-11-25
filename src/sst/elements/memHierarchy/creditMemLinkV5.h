// SnnDL V5 Storage-M4: credit-gated memory link subcomponent.
//
// CreditMemLinkV5 subclasses MemLink and adds a sideband "credit" port.
// The remote fabric ingress grants an initial credit budget during SST
// init (MemFlowCredit::InitialGrant) and returns units after it actually
// forwards a message (MemFlowCredit::ReturnCredit).  trySend() reserves
// one message-slot credit before handing the ORIGINAL event to the data
// link; when credits are insufficient it returns false and the caller
// keeps the event — no fabricated NACK, no loss, no reordering.

#ifndef _MEMHIERARCHY_CREDITMEMLINKV5_H_
#define _MEMHIERARCHY_CREDITMEMLINKV5_H_

#include "sst/elements/memHierarchy/memLink.h"
#include "sst/elements/memHierarchy/memFlowCredit.h"

namespace SST {
namespace MemHierarchy {

class CreditMemLinkV5 : public MemLink {

public:
/* Element Library Info */
    SST_ELI_REGISTER_SUBCOMPONENT(CreditMemLinkV5, "memHierarchy", "CreditMemLinkV5", SST_ELI_ELEMENT_VERSION(1,0,0),
            "Credit-gated memory link for bounded fabric ingress (Storage-M4)", SST::MemHierarchy::MemLinkBase)

#define CREDITMEMLINKV5_ELI_PARAMS MEMLINK_ELI_PARAMS

    SST_ELI_DOCUMENT_PARAMS( { CREDITMEMLINKV5_ELI_PARAMS } )

    SST_ELI_DOCUMENT_PORTS(
        { "port",   "Data port toward the memory fabric", {"memHierarchy.MemEventBase"} },
        { "credit", "Credit return sideband from the memory fabric", {"memHierarchy.MemFlowCredit"} })

    SST_ELI_DOCUMENT_STATISTICS(
        { "credit_stall_attempts", "trySend calls refused for lack of local credits", "units", 1 })

    /* Constructor — signature must match MemLink for MemLinkBase user
     * subcomponent slots (loadUserSubComponent passes the TimeConverter). */
    CreditMemLinkV5(ComponentId_t id, Params &params, TimeConverter* tc);
    virtual ~CreditMemLinkV5() { }

    virtual void init(unsigned int phase) override;
    virtual void complete(unsigned int phase) override;

    /* Admission-controlled send: true = ownership moved to the data link;
     * false = caller keeps the event (no credits consumed). */
    virtual bool trySend(MemEventBase * ev) override;

    /* Legacy callers must never bypass the ingress budget on a
     * credit-controlled port. */
    virtual void send(MemEventBase * ev) override;

    CreditMemLinkV5() { }
    void serialize_order(SST::Core::Serialization::serializer& ser) override;
    ImplementSerializable(SST::MemHierarchy::CreditMemLinkV5)

private:
    void handleCreditEvent(SST::Event* ev);
    void consumeUntimedCredits();

    SST::Link* creditLink_ = nullptr;

    uint64_t granted_    = 0;   // locally available send permission
    uint64_t capacity_   = 0;   // ingress budget learned via InitialGrant
    bool     grantSeen_  = false;

    Statistic<uint64_t>* statCreditStalls_ = nullptr;
};

} //namespace MemHierarchy
} //namespace SST

#endif
