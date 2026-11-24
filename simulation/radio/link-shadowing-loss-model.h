/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2 (R1 validation, review item 2).
 *
 * Link-reciprocal log-normal shadowing for R1, following 3GPP TR 38.901 (ETSI TR 138 901 V16.1.0):
 *  - clause 7.5 Step 4: shadow fading (SF) is a large-scale parameter of each BS-UT link;
 *    "LSPs for different BS-UT links are uncorrelated";
 *  - clause 7.6.5 (multi-frequency simulations): "The parameters generated in Step 4 are the same
 *    for all the frequencies" (same x ~ N(0,1) for every frequency).
 * Hence the FDD downlink (2.12 GHz) and uplink (1.93 GHz) of one eNB-UE link share ONE shadowing
 * value, independent across links. ns-3's LteHelper instantiates separate DL and UL path-loss
 * models with independent shadowing, which departs from clause 7.6.5.
 *
 * Usage: ONE instance is chained (PropagationLossModel::SetNext) after BOTH the DL and the UL
 * ThreeGppUmaPropagationLossModel instances, whose own shadowing is disabled. The frequency-dependent
 * mean path loss therefore stays per direction; only the shadowing term is shared.
 * Stationary UEs: one draw per unordered node pair, cached for the run (no spatial update).
 */

#ifndef LINK_SHADOWING_LOSS_MODEL_H
#define LINK_SHADOWING_LOSS_MODEL_H

#include "ns3/propagation-loss-model.h"
#include "ns3/random-variable-stream.h"

#include <map>
#include <ostream>
#include <utility>

namespace ns3
{

class LinkShadowingLossModel : public PropagationLossModel
{
  public:
    static TypeId GetTypeId();
    LinkShadowingLossModel();
    ~LinkShadowingLossModel() override;

    /** Write every drawn link value: node_a,node_b,shadowing_db (a < b). */
    void Dump(std::ostream& os) const;

  private:
    double DoCalcRxPower(double txPowerDbm, Ptr<MobilityModel> a, Ptr<MobilityModel> b) const override;
    int64_t DoAssignStreams(int64_t stream) override;

    double m_sigmaDb;                                           //!< shadowing std (dB)
    Ptr<NormalRandomVariable> m_normal;                         //!< N(0,1)
    mutable std::map<std::pair<uint32_t, uint32_t>, double> m_cache; //!< unordered link -> S (dB)
};

} // namespace ns3

#endif // LINK_SHADOWING_LOSS_MODEL_H
