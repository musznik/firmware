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
    meshtastic_OnDemandType_RESPONSE_PING = 6,
    meshtastic_OnDemandType_REQUEST_RX_AVG_TIME = 7,
    meshtastic_OnDemandType_RESPONSE_RX_AVG_TIME = 8,
    meshtastic_OnDemandType_REQUEST_PORT_COUNTER_HISTORY = 9,
    meshtastic_OnDemandType_RESPONSE_PORT_COUNTER_HISTORY = 10,
    meshtastic_OnDemandType_REQUEST_PACKET_EXCHANGE_HISTORY = 11,
    meshtastic_OnDemandType_RESPONSE_PACKET_EXCHANGE_HISTORY = 12,
    meshtastic_OnDemandType_REQUEST_AIR_ACTIVITY_HISTORY = 13,
    meshtastic_OnDemandType_RESPONSE_AIR_ACTIVITY_HISTORY = 14
} meshtastic_OnDemandType;

/* Struct definitions */
typedef struct _meshtastic_RxPacketHistory {
    pb_size_t rx_packet_history_count;
    uint32_t rx_packet_history[40];
} meshtastic_RxPacketHistory;

typedef struct _meshtastic_RxAvgTimeHistory {
    pb_size_t rx_avg_history_count;
    uint32_t rx_avg_history[40];
} meshtastic_RxAvgTimeHistory;

typedef struct _meshtastic_PortCounterEntry {
    uint32_t port;
    uint32_t count;
} meshtastic_PortCounterEntry;

typedef struct _meshtastic_PortCountersHistory {
    pb_size_t port_counter_history_count;
    meshtastic_PortCounterEntry port_counter_history[20];
} meshtastic_PortCountersHistory;

typedef struct _meshtastic_AirActivityEntry {
    uint32_t tx_time;
    uint32_t rx_time;
} meshtastic_AirActivityEntry;

typedef struct _meshtastic_AirActivityHistory {
    pb_size_t air_activity_history_count;
    meshtastic_AirActivityEntry air_activity_history[10];
} meshtastic_AirActivityHistory;

typedef struct _meshtastic_NodeEntry {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    uint32_t hops;
} meshtastic_NodeEntry;

typedef struct _meshtastic_ExchangeEntry {
    uint32_t from_node;
    uint32_t to_node;
    uint32_t port_num;
} meshtastic_ExchangeEntry;

typedef struct _meshtastic_ExchangeList {
    pb_size_t exchange_list_count;
    meshtastic_ExchangeEntry exchange_list[12];
} meshtastic_ExchangeList;

typedef struct _meshtastic_NodesList {
    pb_size_t node_list_count;
    meshtastic_NodeEntry node_list[10];
} meshtastic_NodesList;

typedef struct _meshtastic_Ping {
    char dummy_field;
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
        meshtastic_RxAvgTimeHistory rx_avg_time_history;
        meshtastic_PortCountersHistory port_counter_history;
        meshtastic_ExchangeList exchange_packet_log;
        meshtastic_AirActivityHistory air_activity_history;
    } response_data;
} meshtastic_OnDemandResponse;

typedef struct _meshtastic_OnDemand {
    bool has_packet_index;
    uint8_t packet_index;
    bool has_packet_total;
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
#define _meshtastic_OnDemandType_MAX meshtastic_OnDemandType_RESPONSE_AIR_ACTIVITY_HISTORY
#define _meshtastic_OnDemandType_ARRAYSIZE ((meshtastic_OnDemandType)(meshtastic_OnDemandType_RESPONSE_AIR_ACTIVITY_HISTORY+1))












#define meshtastic_OnDemandRequest_request_type_ENUMTYPE meshtastic_OnDemandType

#define meshtastic_OnDemandResponse_response_type_ENUMTYPE meshtastic_OnDemandType



/* Initializer values for message structs */
#define meshtastic_RxPacketHistory_init_default  {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_RxAvgTimeHistory_init_default {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_PortCounterEntry_init_default {0, 0}
#define meshtastic_PortCountersHistory_init_default {0, {meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default, meshtastic_PortCounterEntry_init_default}}
#define meshtastic_AirActivityEntry_init_default {0, 0}
#define meshtastic_AirActivityHistory_init_default {0, {meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default, meshtastic_AirActivityEntry_init_default}}
#define meshtastic_NodeEntry_init_default        {0, "", "", 0, 0, 0}
#define meshtastic_ExchangeEntry_init_default    {0, 0, 0}
#define meshtastic_ExchangeList_init_default     {0, {meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default, meshtastic_ExchangeEntry_init_default}}
#define meshtastic_NodesList_init_default        {0, {meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default, meshtastic_NodeEntry_init_default}}
#define meshtastic_Ping_init_default             {0}
#define meshtastic_OnDemandRequest_init_default  {_meshtastic_OnDemandType_MIN}
#define meshtastic_OnDemandResponse_init_default {_meshtastic_OnDemandType_MIN, 0, {meshtastic_RxPacketHistory_init_default}}
#define meshtastic_OnDemand_init_default         {false, 0, false, 0, 0, {meshtastic_OnDemandRequest_init_default}}
#define meshtastic_RxPacketHistory_init_zero     {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_RxAvgTimeHistory_init_zero    {0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
#define meshtastic_PortCounterEntry_init_zero    {0, 0}
#define meshtastic_PortCountersHistory_init_zero {0, {meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero, meshtastic_PortCounterEntry_init_zero}}
#define meshtastic_AirActivityEntry_init_zero    {0, 0}
#define meshtastic_AirActivityHistory_init_zero  {0, {meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero, meshtastic_AirActivityEntry_init_zero}}
#define meshtastic_NodeEntry_init_zero           {0, "", "", 0, 0, 0}
#define meshtastic_ExchangeEntry_init_zero       {0, 0, 0}
#define meshtastic_ExchangeList_init_zero        {0, {meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero, meshtastic_ExchangeEntry_init_zero}}
#define meshtastic_NodesList_init_zero           {0, {meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero, meshtastic_NodeEntry_init_zero}}
#define meshtastic_Ping_init_zero                {0}
#define meshtastic_OnDemandRequest_init_zero     {_meshtastic_OnDemandType_MIN}
#define meshtastic_OnDemandResponse_init_zero    {_meshtastic_OnDemandType_MIN, 0, {meshtastic_RxPacketHistory_init_zero}}
#define meshtastic_OnDemand_init_zero            {false, 0, false, 0, 0, {meshtastic_OnDemandRequest_init_zero}}

/* Field tags (for use in manual encoding/decoding) */
#define meshtastic_RxPacketHistory_rx_packet_history_tag 1
#define meshtastic_RxAvgTimeHistory_rx_avg_history_tag 1
#define meshtastic_PortCounterEntry_port_tag     1
#define meshtastic_PortCounterEntry_count_tag    2
#define meshtastic_PortCountersHistory_port_counter_history_tag 1
#define meshtastic_AirActivityEntry_tx_time_tag  1
#define meshtastic_AirActivityEntry_rx_time_tag  2
#define meshtastic_AirActivityHistory_air_activity_history_tag 1
#define meshtastic_NodeEntry_node_id_tag         1
#define meshtastic_NodeEntry_long_name_tag       2
#define meshtastic_NodeEntry_short_name_tag      3
#define meshtastic_NodeEntry_last_heard_tag      4
#define meshtastic_NodeEntry_snr_tag             5
#define meshtastic_NodeEntry_hops_tag            6
#define meshtastic_ExchangeEntry_from_node_tag   1
#define meshtastic_ExchangeEntry_to_node_tag     2
#define meshtastic_ExchangeEntry_port_num_tag    3
#define meshtastic_ExchangeList_exchange_list_tag 1
#define meshtastic_NodesList_node_list_tag       1
#define meshtastic_OnDemandRequest_request_type_tag 1
#define meshtastic_OnDemandResponse_response_type_tag 1
#define meshtastic_OnDemandResponse_rx_packet_history_tag 2
#define meshtastic_OnDemandResponse_node_list_tag 3
#define meshtastic_OnDemandResponse_ping_tag     4
#define meshtastic_OnDemandResponse_rx_avg_time_history_tag 5
#define meshtastic_OnDemandResponse_port_counter_history_tag 6
#define meshtastic_OnDemandResponse_exchange_packet_log_tag 7
#define meshtastic_OnDemandResponse_air_activity_history_tag 8
#define meshtastic_OnDemand_packet_index_tag     1
#define meshtastic_OnDemand_packet_total_tag     2
#define meshtastic_OnDemand_request_tag          3
#define meshtastic_OnDemand_response_tag         4

/* Struct field encoding specification for nanopb */
#define meshtastic_RxPacketHistory_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, UINT32,   rx_packet_history,   1)
#define meshtastic_RxPacketHistory_CALLBACK NULL
#define meshtastic_RxPacketHistory_DEFAULT NULL

#define meshtastic_RxAvgTimeHistory_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, UINT32,   rx_avg_history,    1)
#define meshtastic_RxAvgTimeHistory_CALLBACK NULL
#define meshtastic_RxAvgTimeHistory_DEFAULT NULL

#define meshtastic_PortCounterEntry_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   port,              1) \
X(a, STATIC,   SINGULAR, UINT32,   count,             2)
#define meshtastic_PortCounterEntry_CALLBACK NULL
#define meshtastic_PortCounterEntry_DEFAULT NULL

#define meshtastic_PortCountersHistory_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, MESSAGE,  port_counter_history,   1)
#define meshtastic_PortCountersHistory_CALLBACK NULL
#define meshtastic_PortCountersHistory_DEFAULT NULL
#define meshtastic_PortCountersHistory_port_counter_history_MSGTYPE meshtastic_PortCounterEntry

#define meshtastic_AirActivityEntry_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   tx_time,           1) \
X(a, STATIC,   SINGULAR, UINT32,   rx_time,           2)
#define meshtastic_AirActivityEntry_CALLBACK NULL
#define meshtastic_AirActivityEntry_DEFAULT NULL

#define meshtastic_AirActivityHistory_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, MESSAGE,  air_activity_history,   1)
#define meshtastic_AirActivityHistory_CALLBACK NULL
#define meshtastic_AirActivityHistory_DEFAULT NULL
#define meshtastic_AirActivityHistory_air_activity_history_MSGTYPE meshtastic_AirActivityEntry

#define meshtastic_NodeEntry_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   node_id,           1) \
X(a, STATIC,   SINGULAR, STRING,   long_name,         2) \
X(a, STATIC,   SINGULAR, STRING,   short_name,        3) \
X(a, STATIC,   SINGULAR, FIXED32,  last_heard,        4) \
X(a, STATIC,   SINGULAR, FLOAT,    snr,               5) \
X(a, STATIC,   SINGULAR, UINT32,   hops,              6)
#define meshtastic_NodeEntry_CALLBACK NULL
#define meshtastic_NodeEntry_DEFAULT NULL

#define meshtastic_ExchangeEntry_FIELDLIST(X, a) \
X(a, STATIC,   SINGULAR, UINT32,   from_node,         1) \
X(a, STATIC,   SINGULAR, UINT32,   to_node,           2) \
X(a, STATIC,   SINGULAR, UINT32,   port_num,          3)
#define meshtastic_ExchangeEntry_CALLBACK NULL
#define meshtastic_ExchangeEntry_DEFAULT NULL

#define meshtastic_ExchangeList_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, MESSAGE,  exchange_list,     1)
#define meshtastic_ExchangeList_CALLBACK NULL
#define meshtastic_ExchangeList_DEFAULT NULL
#define meshtastic_ExchangeList_exchange_list_MSGTYPE meshtastic_ExchangeEntry

#define meshtastic_NodesList_FIELDLIST(X, a) \
X(a, STATIC,   REPEATED, MESSAGE,  node_list,         1)
#define meshtastic_NodesList_CALLBACK NULL
#define meshtastic_NodesList_DEFAULT NULL
#define meshtastic_NodesList_node_list_MSGTYPE meshtastic_NodeEntry

#define meshtastic_Ping_FIELDLIST(X, a) \

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
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,ping,response_data.ping),   4) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,rx_avg_time_history,response_data.rx_avg_time_history),   5) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,port_counter_history,response_data.port_counter_history),   6) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,exchange_packet_log,response_data.exchange_packet_log),   7) \
X(a, STATIC,   ONEOF,    MESSAGE,  (response_data,air_activity_history,response_data.air_activity_history),   8)
#define meshtastic_OnDemandResponse_CALLBACK NULL
#define meshtastic_OnDemandResponse_DEFAULT NULL
#define meshtastic_OnDemandResponse_response_data_rx_packet_history_MSGTYPE meshtastic_RxPacketHistory
#define meshtastic_OnDemandResponse_response_data_node_list_MSGTYPE meshtastic_NodesList
#define meshtastic_OnDemandResponse_response_data_ping_MSGTYPE meshtastic_Ping
#define meshtastic_OnDemandResponse_response_data_rx_avg_time_history_MSGTYPE meshtastic_RxAvgTimeHistory
#define meshtastic_OnDemandResponse_response_data_port_counter_history_MSGTYPE meshtastic_PortCountersHistory
#define meshtastic_OnDemandResponse_response_data_exchange_packet_log_MSGTYPE meshtastic_ExchangeList
#define meshtastic_OnDemandResponse_response_data_air_activity_history_MSGTYPE meshtastic_AirActivityHistory

#define meshtastic_OnDemand_FIELDLIST(X, a) \
X(a, STATIC,   OPTIONAL, UINT32,   packet_index,      1) \
X(a, STATIC,   OPTIONAL, UINT32,   packet_total,      2) \
X(a, STATIC,   ONEOF,    MESSAGE,  (variant,request,variant.request),   3) \
X(a, STATIC,   ONEOF,    MESSAGE,  (variant,response,variant.response),   4)
#define meshtastic_OnDemand_CALLBACK NULL
#define meshtastic_OnDemand_DEFAULT NULL
#define meshtastic_OnDemand_variant_request_MSGTYPE meshtastic_OnDemandRequest
#define meshtastic_OnDemand_variant_response_MSGTYPE meshtastic_OnDemandResponse

extern const pb_msgdesc_t meshtastic_RxPacketHistory_msg;
extern const pb_msgdesc_t meshtastic_RxAvgTimeHistory_msg;
extern const pb_msgdesc_t meshtastic_PortCounterEntry_msg;
extern const pb_msgdesc_t meshtastic_PortCountersHistory_msg;
extern const pb_msgdesc_t meshtastic_AirActivityEntry_msg;
extern const pb_msgdesc_t meshtastic_AirActivityHistory_msg;
extern const pb_msgdesc_t meshtastic_NodeEntry_msg;
extern const pb_msgdesc_t meshtastic_ExchangeEntry_msg;
extern const pb_msgdesc_t meshtastic_ExchangeList_msg;
extern const pb_msgdesc_t meshtastic_NodesList_msg;
extern const pb_msgdesc_t meshtastic_Ping_msg;
extern const pb_msgdesc_t meshtastic_OnDemandRequest_msg;
extern const pb_msgdesc_t meshtastic_OnDemandResponse_msg;
extern const pb_msgdesc_t meshtastic_OnDemand_msg;

/* Defines for backwards compatibility with code written before nanopb-0.4.0 */
#define meshtastic_RxPacketHistory_fields &meshtastic_RxPacketHistory_msg
#define meshtastic_RxAvgTimeHistory_fields &meshtastic_RxAvgTimeHistory_msg
#define meshtastic_PortCounterEntry_fields &meshtastic_PortCounterEntry_msg
#define meshtastic_PortCountersHistory_fields &meshtastic_PortCountersHistory_msg
#define meshtastic_AirActivityEntry_fields &meshtastic_AirActivityEntry_msg
#define meshtastic_AirActivityHistory_fields &meshtastic_AirActivityHistory_msg
#define meshtastic_NodeEntry_fields &meshtastic_NodeEntry_msg
#define meshtastic_ExchangeEntry_fields &meshtastic_ExchangeEntry_msg
#define meshtastic_ExchangeList_fields &meshtastic_ExchangeList_msg
#define meshtastic_NodesList_fields &meshtastic_NodesList_msg
#define meshtastic_Ping_fields &meshtastic_Ping_msg
#define meshtastic_OnDemandRequest_fields &meshtastic_OnDemandRequest_msg
#define meshtastic_OnDemandResponse_fields &meshtastic_OnDemandResponse_msg
#define meshtastic_OnDemand_fields &meshtastic_OnDemand_msg

/* Maximum encoded size of messages (where known) */
#define MESHTASTIC_MESHTASTIC_ONDEMAND_PB_H_MAX_SIZE meshtastic_OnDemand_size
#define meshtastic_AirActivityEntry_size         12
#define meshtastic_AirActivityHistory_size       140
#define meshtastic_ExchangeEntry_size            18
#define meshtastic_ExchangeList_size             240
#define meshtastic_NodeEntry_size                69
#define meshtastic_NodesList_size                710
#define meshtastic_OnDemandRequest_size          2
#define meshtastic_OnDemandResponse_size         715
#define meshtastic_OnDemand_size                 724
#define meshtastic_Ping_size                     0
#define meshtastic_PortCounterEntry_size         12
#define meshtastic_PortCountersHistory_size      280
#define meshtastic_RxAvgTimeHistory_size         240
#define meshtastic_RxPacketHistory_size          240

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
