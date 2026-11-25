// SnnDL V5 Storage-M4: credit-gated memory link subcomponent.
// See creditMemLinkV5.h for the protocol summary.

#include "sst/elements/memHierarchy/creditMemLinkV5.h"

using namespace SST;
using namespace SST::MemHierarchy;

CreditMemLinkV5::CreditMemLinkV5(ComponentId_t id, Params &params, TimeConverter* tc)
    : MemLink(id, params, tc) {

    creditLink_ = configureLink(
        "credit", new Event::Handler2<CreditMemLinkV5, &CreditMemLinkV5::handleCreditEvent>(this));
    if (!creditLink_) {
        dbg.fatal(CALL_INFO, -1,
            "%s: CreditMemLinkV5 requires a connected 'credit' port\n", getName().c_str());
    }

    statCreditStalls_ = registerStatistic<uint64_t>("credit_stall_attempts");
}

void CreditMemLinkV5::consumeUntimedCredits() {
    SST::Event* ev;
    while ((ev = creditLink_->recvUntimedData()) != nullptr) {
        MemFlowCredit* credit = dynamic_cast<MemFlowCredit*>(ev);
        if (credit == nullptr || credit->kind != MemFlowCreditKind::InitialGrant) {
            dbg.fatal(CALL_INFO, -1,
                "%s: unexpected untimed event on 'credit' port (only InitialGrant is legal)\n",
                getName().c_str());
        }
        if (grantSeen_) {
            dbg.fatal(CALL_INFO, -1,
                "%s: duplicate InitialGrant on 'credit' port — capacity would be granted twice\n",
                getName().c_str());
        }
        grantSeen_ = true;
        capacity_  = credit->units;
        granted_   = credit->units;
        delete credit;
    }
}

void CreditMemLinkV5::init(unsigned int phase) {
    /* Base MemLink initialization (region/endpoint discovery on the data
     * port) is unchanged; the credit sideband only carries grants. */
    MemLink::init(phase);
    consumeUntimedCredits();
}

void CreditMemLinkV5::complete(unsigned int phase) {
    MemLink::complete(phase);
    consumeUntimedCredits();
}

void CreditMemLinkV5::handleCreditEvent(SST::Event* ev) {
    MemFlowCredit* credit = dynamic_cast<MemFlowCredit*>(ev);
    if (credit == nullptr || credit->kind != MemFlowCreditKind::ReturnCredit) {
        dbg.fatal(CALL_INFO, -1,
            "%s: unexpected timed event on 'credit' port (only ReturnCredit is legal)\n",
            getName().c_str());
    }
    granted_ += credit->units;
    if (granted_ > capacity_) {
        dbg.fatal(CALL_INFO, -1,
            "%s: credit over-return — granted (%" PRIu64 ") exceeds ingress"
            " capacity (%" PRIu64 "); the fabric returned more units than it"
            " ever reserved\n", getName().c_str(), granted_, capacity_);
    }
    delete credit;
}

bool CreditMemLinkV5::trySend(MemEventBase* ev) {
    if (!grantSeen_) {
        dbg.fatal(CALL_INFO, -1,
            "%s: trySend before InitialGrant — the connected fabric did not"
            " publish ingress capacity (wiring or flow_control mismatch)\n",
            getName().c_str());
    }
    const uint64_t k = memFlowMessageSlots();

    if (k > capacity_) {
        dbg.fatal(CALL_INFO, -1,
            "%s: message needs %" PRIu64 " credits but ingress capacity is only %" PRIu64
            " — message cannot ever fit this fabric port\n",
            getName().c_str(), k, capacity_);
    }
    if (k > granted_) {
        statCreditStalls_->addData(1);
        return false;
    }
    granted_ -= k;
    MemLink::send(ev);
    return true;
}

void CreditMemLinkV5::send(MemEventBase* ev) {
    /* A legacy caller using void send() on a credit-controlled port is a
     * wiring error — it would bypass ingress capacity silently. */
    if (!trySend(ev)) {
        dbg.fatal(CALL_INFO, -1,
            "%s: send() called on a credit-controlled port without sufficient credits;"
            " callers must use trySend()\n", getName().c_str());
    }
}

void CreditMemLinkV5::serialize_order(SST::Core::Serialization::serializer& ser) {
    MemLink::serialize_order(ser);
    SST_SER(creditLink_);
    SST_SER(granted_);
    SST_SER(capacity_);
    SST_SER(grantSeen_);
}
