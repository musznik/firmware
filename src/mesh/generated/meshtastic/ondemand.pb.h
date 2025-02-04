/* Automatically generated nanopb header */
/* Generated by nanopb-0.4.9 */

#ifndef PB_MESHTASTIC_MESHTASTIC_ONDEMAND_PB_H_INCLUDED
#define PB_MESHTASTIC_MESHTASTIC_ONDEMAND_PB_H_INCLUDED
#include <pb.h>

#if PB_PROTO_HEADER_VERSION != 40
#error Regenerate this file with the current version of nanopb generator.
#endif

/* Enum definitions */
typedef enum _meshtastic_OnDemandType {
    meshtastic_OnDemandType_UNKNOWN_TYPE = 0,
    meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY = 1,
    meshtastic_OnDemandType_RESPONSE_PACKET_RX_HISTORY = 2,
    meshtastic_OnDemandType_REQUEST_NODES_ONLINE = 3,
    meshtastic_OnDemandType_RESPONSE_NODES_ONLINE = 4,
    meshtastic_OnDemandType_REQUEST_PING = 5,
    meshtastic_OnDemandType_RESPONSE_PING = 6
} meshtastic_OnDemandType;

/* Struct definitions */
typedef struct _meshtastic_RxPacketHistory {
    pb_size_t rx_packet_history_count;
    uint32_t rx_packet_history[40];
} meshtastic_RxPacketHistory;

typedef struct _meshtastic_NodeEntry {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
} meshtastic_NodeEntry;

typedef struct _meshtastic_NodesList {
    pb_size_t node_list_count;
    meshtastic_NodeEntry node_list[10];
} meshtastic_NodesList;

typedef struct _meshtastic_Ping {
    bool direct;
} meshtastic_Ping;

typedef struct _meshtastic_OnDemandRequest {
    meshtastic_OnDemandType request_type;
} meshtastic_OnDemandRequest;

typedef struct _meshtastic_OnDemandResponse {
    meshtastic_OnDemandType response_type;
    pb_size_t which_response_data;
    union {
        meshtastic_RxPacketHistory rx_packet_history;
        meshtastic_NodesList node_list;
        meshtastic_Ping ping;
    } response_data;
} meshtastic_OnDemandResponse;

typedef struct _meshtastic_OnDemand {
    uint8_t packet_index;
    uint8_t packet_total;
    pb_size_t which_variant;
    union {
        meshtastic_OnDemandRequest request;
        meshtastic_OnDemandResponse response;
    } variant;
} meshtastic_OnDemand;


#ifdef __cplusplus
extern "C" {
#endif

/* Helper constants for enums */
#define _meshtastic_OnDemandType_MIN meshtastic_OnDemandType_UNKNOWN_TYPE
#define _meshtastic_OnDemandType_MAX meshtastic_OnDemandType_RESPONSE_PING
#define _meshtastic_OnDemandType_ARRAYSIZE ((meshtastic_OnDemandType)(meshtastic_OnDemandType_RESPONSE_PING+1))





#define meshtastic_OnDemandRequest_request_type_ENUMTYPE meshtastic_OnDemandType

#define meshtastic_OnDemandResponse_response_type_ENUMTYPE meshtastic_OnDemandType



/* Initializer values for message structs */
#define meshtastic_RxPacketHistory_init_default  {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_NodeEntry_init_default        {0, "", "", 0, 0}
#define meshtastic_NodesList_init_default        {0, {meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default}}
#define meshtastic_Ping_init_default             {0}
#define meshtastic_OnDemandRequest_init_default  {_meshtastic_OnDemandType_MIN}
#define meshtastic_OnDemandResponse_init_default {_meshtastic_OnDemandType_MIN, 0, {meshtastic_RxPacketHistory_init_default}}
#define meshtastic_OnDemand_init_default         {0, 0, 0, {meshtastic_OnDemandRequest_init_default}}
#define meshtastic_RxPacketHistory_init_zero     {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_NodeEntry_init_zero           {0, "", "", 0, 0}
#define meshtastic_NodesList_init_zero           {0, {meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero}}
#define meshtastic_Ping_init_zero                {0}
#define meshtastic_OnDemandRequest_init_zero     {_meshtastic_OnDemandType_MIN}
#define meshtastic_OnDemandResponse_init_zero    {_meshtastic_OnDemandType_MIN, 0, {meshtastic_RxPacketHistory_init_zero}}
#define meshtastic_OnDemand_init_zero            {0, 0, 0, {meshtastic_OnDemandRequest_init_zero}}

/* Field tags (for use in manual encoding/decoding) */
#define meshtastic_RxPacketHistory_rx_packet_history_tag 1
#define meshtastic_NodeEntry_node_id_tag         1
#define meshtastic_NodeEntry_long_name_tag       2
#define meshtastic_NodeEntry_short_name_tag      3
#define meshtastic_NodeEntry_last_heard_tag      4
#define meshtastic_NodeEntry_snr_tag             5
#define meshtastic_NodesList_node_list_tag       1
#define meshtastic_Ping_direct_tag               1
#define meshtastic_OnDemandRequest_request_type_tag 1
#define meshtastic_OnDemandResponse_response_type_tag 1
#define meshtastic_OnDemandResponse_rx_packet_history_tag 2
#define meshtastic_OnDemandResponse_node_list_tag 3
#define meshtastic_OnDemandResponse_ping_tag     4
#define meshtastic_OnDemand_packet_index_tag     1
#define meshtastic_OnDemand_packet_total_tag     2
#define meshtastic_OnDemand_request_tag          3
#define meshtastic_OnDemand_response_tag         4

/* Struct field encoding specification for nanopb */
#define meshtastic_RxPacketHistory_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, UINT32,   rx_packet_history,   1)
#define meshtastic_RxPacketHistory_CALLBACK NULL
#define meshtastic_RxPacketHistory_DEFAULT NULL

#define meshtastic_NodeEntry_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   node_id,           1) \
X(a, STATIC,   SINGULAR, STRING,   long_name,         2) \
X(a, STATIC,   SINGULAR, STRING,   short_name,        3) \
X(a, STATIC,   SINGULAR, FIXED32,  last_heard,        4) \
X(a, STATIC,   SINGULAR, FLOAT,    snr,               5)
#define meshtastic_NodeEntry_CALLBACK NULL
#define meshtastic_NodeEntry_DEFAULT NULL

#define meshtastic_NodesList_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, MESSAGE,  node_list,         1)
#define meshtastic_NodesList_CALLBACK NULL
#define meshtastic_NodesList_DEFAULT NULL
#define meshtastic_NodesList_node_list_MSGTYPE meshtastic_NodeEntry

#define meshtastic_Ping_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, BOOL,     direct,            1)
#define meshtastic_Ping_CALLBACK NULL
#define meshtastic_Ping_DEFAULT NULL

#define meshtastic_OnDemandRequest_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UENUM,    request_type,      1)
#define meshtastic_OnDemandRequest_CALLBACK NULL
#define meshtastic_OnDemandRequest_DEFAULT NULL

#define meshtastic_OnDemandResponse_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UENUM,    response_type,     1) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,rx_packet_history,response_data.rx_packet_history),   2) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,node_list,response_data.node_list),   3) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,ping,response_data.ping),   4)
#define meshtastic_OnDemandResponse_CALLBACK NULL
#define meshtastic_OnDemandResponse_DEFAULT NULL
#define meshtastic_OnDemandResponse_response_data_rx_packet_history_MSGTYPE meshtastic_RxPacketHistory
#define meshtastic_OnDemandResponse_response_data_node_list_MSGTYPE meshtastic_NodesList
#define meshtastic_OnDemandResponse_response_data_ping_MSGTYPE meshtastic_Ping

#define meshtastic_OnDemand_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   packet_index,      1) \
X(a, STATIC,   SINGULAR, UINT32,   packet_total,      2) \
X(a, STATIC,   ONEOF,    MESSAGE,  (variant,request,variant.request),   3) \
X(a, STATIC,   ONEOF,    MESSAGE,  (variant,response,variant.response),   4)
#define meshtastic_OnDemand_CALLBACK NULL
#define meshtastic_OnDemand_DEFAULT NULL
#define meshtastic_OnDemand_variant_request_MSGTYPE meshtastic_OnDemandRequest
#define meshtastic_OnDemand_variant_response_MSGTYPE meshtastic_OnDemandResponse

extern const pb_msgdesc_t meshtastic_RxPacketHistory_msg;
extern const pb_msgdesc_t meshtastic_NodeEntry_msg;
extern const pb_msgdesc_t meshtastic_NodesList_msg;
extern const pb_msgdesc_t meshtastic_Ping_msg;
extern const pb_msgdesc_t meshtastic_OnDemandRequest_msg;
extern const pb_msgdesc_t meshtastic_OnDemandResponse_msg;
extern const pb_msgdesc_t meshtastic_OnDemand_msg;

/* Defines for backwards compatibility with code written before nanopb-0.4.0 */
#define meshtastic_RxPacketHistory_fields &meshtastic_RxPacketHistory_msg
#define meshtastic_NodeEntry_fields &meshtastic_NodeEntry_msg
#define meshtastic_NodesList_fields &meshtastic_NodesList_msg
#define meshtastic_Ping_fields &meshtastic_Ping_msg
#define meshtastic_OnDemandRequest_fields &meshtastic_OnDemandRequest_msg
#define meshtastic_OnDemandResponse_fields &meshtastic_OnDemandResponse_msg
#define meshtastic_OnDemand_fields &meshtastic_OnDemand_msg

/* Maximum encoded size of messages (where known) */
#define MESHTASTIC_MESHTASTIC_ONDEMAND_PB_H_MAX_SIZE meshtastic_OnDemand_size
#define meshtastic_NodeEntry_size                63
#define meshtastic_NodesList_size                650
#define meshtastic_OnDemandRequest_size          2
#define meshtastic_OnDemandResponse_size         655
#define meshtastic_OnDemand_size                 664
#define meshtastic_Ping_size                     2
#define meshtastic_RxPacketHistory_size          240

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
