#include "HeardModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "mesh/generated/meshtastic/heard.pb.h" //fw+
#include <pb.h>            //fw+
#include <pb_encode.h>     //fw+

//fw+
HeardModule::HeardModule() : ProtobufModule<meshtastic_HeardReport>("heard", meshtastic_PortNum_HEARD_APP, &meshtastic_HeardReport_msg) //fw+
{
    this->isPromiscuous = true; // we need to observe text broadcasts
    // Pull defaults from nodemodadmin if available
    if (moduleConfig.has_nodemodadmin) {
        //fw+ leave 0% unless user enables explicitly after protobufs are added
        if (moduleConfig.nodemodadmin.heard_window_ms) windowMs = moduleConfig.nodemodadmin.heard_window_ms;
        if (moduleConfig.nodemodadmin.heard_max_reports_per_agg) maxReportsPerAgg = moduleConfig.nodemodadmin.heard_max_reports_per_agg;
        if (moduleConfig.nodemodadmin.heard_max_active_keys) maxActiveKeys = moduleConfig.nodemodadmin.heard_max_active_keys;
        if (moduleConfig.nodemodadmin.heard_sampling_enabled_texts) samplePercentTexts = (uint8_t)moduleConfig.nodemodadmin.heard_sample_percent_texts;
    }
}

//fw+
void HeardModule::alterReceivedProtobuf(meshtastic_MeshPacket &p, meshtastic_HeardReport *decoded)
{
    // Only observe text broadcast packets
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return;
    if (p.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return;
    if (!isBroadcast(p.to)) return;

    // Sampling path
    if (shouldSampleTextBroadcast(&p)) {
        maybeScheduleHeard(&p);
    }
    // Track last text broadcast id for potential assist
    if (lastText.packet_id != p.id) {
        lastText.packet_id = p.id;
        lastText.first_seen_ms = nowMs();
        lastText.rebroadcasts = 0;
    }

    // Aggregation path will be handled when HEARD frames are received on our port
    // If this is a HEARD frame, try to decode and aggregate passively
    if (p.decoded.portnum == meshtastic_PortNum_HEARD_APP) {
        meshtastic_HeardReport r = meshtastic_HeardReport_init_zero;
        if (pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_HeardReport_msg, &r)) {
            // Passive aggregation for reports in transit (even if not addressed to us)
            ingestAndMaybeAggregate(&p);
        } else {
            meshtastic_HeardAggregate a = meshtastic_HeardAggregate_init_zero;
            if (pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_HeardAggregate_msg, &a)) {
                // For aggregates we don't need to aggregate further; allow normal forwarding
            }
        }
    }
}

//fw+
int32_t HeardModule::runOnce()
{
    // Time-based flushing of aggregates
    uint32_t now = nowMs();
    for (auto it = aggregates.begin(); it != aggregates.end();) {
        if (it->second.expireMs && now >= it->second.expireMs) {
            flushAggregate(it->first);
            it = aggregates.erase(it);
        } else {
            ++it;
        }
    }
    return INT32_MAX; // default sleep
}

//fw+
bool HeardModule::shouldSampleTextBroadcast(const meshtastic_MeshPacket *p) const
{
    if (samplePercentTexts == 0) return false;
    // deterministic hash on (packet id, our node id)
    uint32_t h = 1469598103u;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
    mix(p->id);
    mix(nodeDB->getNodeNum());
    return (h % 100) < samplePercentTexts;
}

//fw+
void HeardModule::maybeScheduleHeard(const meshtastic_MeshPacket *p)
{
    // Build HeardReport and schedule unicast to original source after random jitter within window
    meshtastic_HeardReport rep = meshtastic_HeardReport_init_zero;
    rep.original_source = getFrom(p);
    rep.original_packet_id = p->id;
    rep.receiver = nodeDB->getNodeNum();
    rep.hop_count = computeHopCount(p);
    rep.rx_rssi = p->rx_rssi;
    rep.rx_snr_q4 = (int32_t)(p->rx_snr * 4.0f);
    rep.rx_ms = nowMs();

    meshtastic_MeshPacket *mp = router->allocForSending();
    if (!mp) return;
    mp->to = getFrom(p); // send to original source for now (sink=SOURCE)
    mp->decoded.portnum = meshtastic_PortNum_HEARD_APP;
    mp->decoded.want_response = false;
    if (windowMs) mp->tx_after = nowMs() + (random(windowMs + 1));
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    mp->decoded.payload.size = pb_encode_to_bytes(mp->decoded.payload.bytes, sizeof(mp->decoded.payload.bytes), &meshtastic_HeardReport_msg, &rep);
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
}

//fw+
void HeardModule::ingestAndMaybeAggregate(const meshtastic_MeshPacket *p)
{
    meshtastic_HeardReport r = meshtastic_HeardReport_init_zero;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_HeardReport_msg, &r)) return;
    OriginalKey key{r.original_source, r.original_packet_id};
    // Enforce active keys cap
    if (aggregates.find(key) == aggregates.end()) {
        if (aggregates.size() >= maxActiveKeys) return;
        aggregates[key] = AggregationState{};
        aggregates[key].expireMs = nowMs() + windowMs;
    }
    auto &st = aggregates[key];
    // Deduplicate by receiver
    for (const auto &ex : st.reports) if (ex.receiver == r.receiver) return;
    ReportLite rl{r.receiver, r.rx_rssi, (float)r.rx_snr_q4 / 4.0f, r.hop_count, r.rx_ms};
    st.reports.push_back(rl);
    if (st.reports.size() >= maxReportsPerAgg) {
        flushAggregate(key);
        aggregates.erase(key);
        // Consider assist decision when aggregate closed
        maybeDecideAssist(key);
    }
}

//fw+
void HeardModule::flushAggregate(const OriginalKey &k)
{
    auto it = aggregates.find(k);
    if (it == aggregates.end()) return;
    const auto &st = it->second;
    meshtastic_HeardAggregate agg = meshtastic_HeardAggregate_init_zero;
    agg.original_source = k.src;
    agg.original_packet_id = k.pkt;

    struct AggCtx { const std::vector<ReportLite>* v; uint32_t src; uint32_t pkt; } ctx{ &st.reports, k.src, k.pkt };

    // Encoder callback for repeated reports
    auto encodeReports = [](pb_ostream_t *stream, const pb_field_t *field, void * const *arg) -> bool {
        const AggCtx *c = static_cast<const AggCtx *>(*arg);
        const auto &vec = *c->v;
        for (size_t i = 0; i < vec.size(); ++i) {
            meshtastic_HeardReport r = meshtastic_HeardReport_init_zero;
            r.original_source = c->src;
            r.original_packet_id = c->pkt;
            r.receiver = vec[i].receiver;
            r.hop_count = vec[i].hop_count;
            r.rx_rssi = vec[i].rx_rssi;
            r.rx_snr_q4 = (int32_t)(vec[i].rx_snr * 4.0f);
            r.rx_ms = vec[i].rx_ms;
            if (!pb_encode_tag_for_field(stream, field)) return false;
            if (!pb_encode_submessage(stream, &meshtastic_HeardReport_msg, &r)) return false;
        }
        return true;
    };

    agg.reports.funcs.encode = encodeReports;
    agg.reports.arg = &ctx;

    meshtastic_MeshPacket *mp = router->allocForSending();
    if (!mp) return;
    mp->to = k.src; // sink=SOURCE for now
    mp->decoded.portnum = meshtastic_PortNum_HEARD_APP;
    mp->decoded.want_response = false;
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    mp->decoded.payload.size = pb_encode_to_bytes(mp->decoded.payload.bytes, sizeof(mp->decoded.payload.bytes), &meshtastic_HeardAggregate_msg, &agg);
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    // Also consider assist decision after sending aggregate due to timeout
    maybeDecideAssist(k);
}

//fw+
uint32_t HeardModule::computeHopCount(const meshtastic_MeshPacket *p) const
{
    if (p->hop_start != 0 && p->hop_limit <= p->hop_start) {
        return (uint32_t)(p->hop_start - p->hop_limit);
    }
    return 0;
}

//fw+
uint32_t HeardModule::nowMs() const { return millis(); }

//fw+ simple local assist decision: if few reports and ring likely high, emit control assist
void HeardModule::maybeDecideAssist(const OriginalKey &k)
{
    if (k.pkt != lastText.packet_id) return;
    if (lastText.rebroadcasts >= 2) return; // cap assists
    // Heuristic: if we observed very few HEARD locally, and czas > window, do assist
    auto it = aggregates.find(k);
    size_t localCount = 0;
    if (it != aggregates.end()) localCount = it->second.reports.size();
    if (nowMs() - lastText.first_seen_ms < windowMs) return;
    if (localCount >= 2) return; // enough local reports
    sendAssistControl(k.pkt);
    lastText.rebroadcasts++;
}

//fw+ emit HeardAssist control message with throttle parameters
void HeardModule::sendAssistControl(uint32_t ref_packet_id)
{
    meshtastic_HeardAssist assist = meshtastic_HeardAssist_init_zero;
    assist.assist_ref_packet_id = ref_packet_id;
    // percent_per_ring as callback encoder for [1..7]
    struct PctCtx { uint32_t vals[8]; } ctx{};
    ctx.vals[1] = 1; ctx.vals[2] = 1; ctx.vals[3] = 1; ctx.vals[4] = 2; ctx.vals[5] = 2; ctx.vals[6] = 3; ctx.vals[7] = 3;
    auto encodePercents = [](pb_ostream_t *stream, const pb_field_t *field, void * const *arg) -> bool {
        const PctCtx *c = static_cast<const PctCtx *>(*arg);
        for (int r = 1; r <= 7; ++r) {
            uint32_t v = c->vals[r];
            if (!pb_encode_tag_for_field(stream, field)) return false;
            if (!pb_encode_varint(stream, v)) return false;
        }
        return true;
    };
    assist.percent_per_ring.funcs.encode = encodePercents;
    assist.percent_per_ring.arg = &ctx;
    assist.jitter_base_ms = 40;
    assist.jitter_slope_ms = 120;
    assist.scope_min_ring = 2;

    meshtastic_MeshPacket *mp = router->allocForSending();
    if (!mp) return;
    mp->to = NODENUM_BROADCAST; // control as broadcast; only supporting nodes will apply
    mp->decoded.portnum = meshtastic_PortNum_HEARD_APP;
    mp->decoded.want_response = false;
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    mp->decoded.payload.size = pb_encode_to_bytes(mp->decoded.payload.bytes, sizeof(mp->decoded.payload.bytes), &meshtastic_HeardAssist_msg, &assist);
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
}


