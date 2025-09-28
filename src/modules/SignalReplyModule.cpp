#include "SignalReplyModule.h"
#include "MeshService.h"
#include "configuration.h"

SignalReplyModule *signalReplyModule;

//fw+ Small helpers to avoid std::string allocation and heavy libc where possible
static inline char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

//fw+ Compare a bytes buffer to a literal, case-insensitive; avoids transient std::string
static bool equalsLiteralCaseInsensitive(const uint8_t *data, size_t len, const char *literal)
{
    if (!data || !literal)
        return false;
    size_t litlen = strlen(literal);
    if (len != litlen)
        return false;
    for (size_t i = 0; i < len; ++i)
    {
        if (asciiLower(static_cast<char>(data[i])) != asciiLower(literal[i]))
            return false;
    }
    return true;
}

ProcessMessage SignalReplyModule::handleReceived(const meshtastic_MeshPacket &currentRequest)
{
    auto &p = currentRequest.decoded;
    //fw+ Avoid dynamic allocations when checking for the "pinger" trigger
    bool isPinger = equalsLiteralCaseInsensitive(p.payload.bytes, p.payload.size, "pinger");

    //This condition is meant to reply to message containing request "pinger"
    if ( isPinger && currentRequest.from != 0x0 && currentRequest.from != nodeDB->getNodeNum())
    {
        int hopLimit = currentRequest.hop_limit;
        int hopStart = currentRequest.hop_start;

        //fw+ Ensure enough space for 32-bit unsigned decimal + NUL
        char idSender[11];
        char idReceipient[11];
        snprintf(idSender, sizeof(idSender), "%d", currentRequest.from);
        snprintf(idReceipient, sizeof(idReceipient), "%d", nodeDB->getNodeNum());

        char messageReply[200];
        //fw+ Null checks to avoid potential dereference crashes
        meshtastic_NodeInfoLite *nodeSender = nodeDB->getMeshNode(currentRequest.from);
        const char *username = (nodeSender && nodeSender->has_user) ? nodeSender->user.short_name : idSender;
        meshtastic_NodeInfoLite *nodeReceiver = nodeDB->getMeshNode(nodeDB->getNodeNum());
        const char *usernameja = (nodeReceiver && nodeReceiver->has_user) ? nodeReceiver->user.short_name : idReceipient;

        //LOG_ERROR("SignalReplyModule::handleReceived(): '%s' from %s.", messageRequest, username);

        uint8_t hopsUsed = hopStart < hopLimit ? config.lora.hop_limit : hopStart - hopLimit;
        if (hopLimit != hopStart)
        {
            snprintf(messageReply, sizeof(messageReply), "%s: indirect via %d nodes!", username, (hopsUsed));
        }
        else
        {
            //fw+ Avoid printf float formatting to save flash; render SNR with integers
            int snrTenths = static_cast<int>((currentRequest.rx_snr * 10.0f) + (currentRequest.rx_snr >= 0 ? 0.5f : -0.5f));
            int snrWhole = snrTenths / 10;
            int snrFrac = snrTenths % 10;
            if (snrFrac < 0)
                snrFrac = -snrFrac;
            snprintf(messageReply, sizeof(messageReply), "'%s'->'%s' : RSSI %d dBm, SNR %d.%d dB (@%s).", username, usernameja, currentRequest.rx_rssi, snrWhole, snrFrac, usernameja);
        }

        auto reply = allocDataPacket();
        reply->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        //fw+ Bound the payload size to the destination buffer capacity
        size_t msgLen = strnlen(messageReply, sizeof(messageReply));
        size_t capacity = sizeof(reply->decoded.payload.bytes);
        if (msgLen > capacity)
            msgLen = capacity;
        reply->decoded.payload.size = msgLen;
        reply->from = getFrom(&currentRequest);
        reply->to = currentRequest.from;
        reply->channel = currentRequest.channel;
        reply->want_ack = (currentRequest.from != 0) ? currentRequest.want_ack : false;
        if (currentRequest.priority == meshtastic_MeshPacket_Priority_UNSET)
        {
            reply->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        }
        reply->id = generatePacketId();
        memcpy(reply->decoded.payload.bytes, messageReply, reply->decoded.payload.size);
        service->handleToRadio(*reply);
    }
    notifyObservers(&currentRequest);
    return ProcessMessage::CONTINUE;
}

meshtastic_MeshPacket *SignalReplyModule::allocReply()
{
    assert(currentRequest); // should always be !NULL
    const char *replyStr = "Msg Received";
    auto reply = allocDataPacket();                 // Allocate a packet for sending
    reply->decoded.payload.size = strlen(replyStr); // You must specify how many bytes are in the reply
    memcpy(reply->decoded.payload.bytes, replyStr, reply->decoded.payload.size);
    return reply;
}

bool SignalReplyModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}
