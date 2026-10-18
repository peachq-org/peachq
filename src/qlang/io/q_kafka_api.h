/* q_kafka_api — private, transcribed subset of librdkafka's C API.
 *
 * Transcribed from the BSD-2-licensed `rdkafka.h` of librdkafka 1.8.0 (github.com/edenhill/librdkafka).
 * Deliberately NOT #include'd: `.kafka` reaches librdkafka ONLY via dlopen + a dlsym'd fn-pointer table (the
 * q_duckdb_api.h pattern), so a plain `make` compiles with no library present and `make win` links.  Every struct
 * here is ABI: the layouts are the header's, in its field order. */
#ifndef Q_KAFKA_API_H
#define Q_KAFKA_API_H

#include <stdint.h>
#include <stddef.h>

typedef struct rd_kafka_s         rd_kafka_t;
typedef struct rd_kafka_topic_s   rd_kafka_topic_t;
typedef struct rd_kafka_conf_s    rd_kafka_conf_t;
typedef struct rd_kafka_queue_s   rd_kafka_queue_t;
typedef struct rd_kafka_headers_s rd_kafka_headers_t;
typedef struct rd_kafka_error_s   rd_kafka_error_t;
typedef struct rd_kafka_topic_conf_s rd_kafka_topic_conf_t;

typedef int rd_kafka_resp_err_t;   /* the enum; 0 = RD_KAFKA_RESP_ERR_NO_ERROR, negatives local */
typedef int rd_kafka_conf_res_t;   /* 0 = RD_KAFKA_CONF_OK */
typedef int rd_kafka_type_t;       /* 0 producer, 1 consumer */
typedef int rd_kafka_timestamp_type_t;

enum { KFK_PRODUCER = 0, KFK_CONSUMER = 1 };
enum { KFK_CONF_OK = 0 };
enum { KFK_ERR_NO_ERROR = 0, KFK_ERR_PARTITION_EOF = -191, KFK_ERR_UNKNOWN_PARTITION = -190,
       KFK_ERR_UNKNOWN_TOPIC = -188, KFK_ERR_TIMED_OUT = -185, KFK_ERR_NO_OFFSET = -168,
       KFK_ERR_UNKNOWN_TOPIC_OR_PART = 3 };
enum { KFK_TIMESTAMP_NOT_AVAILABLE = 0 };
enum { KFK_VTYPE_END = 0, KFK_VTYPE_TOPIC = 1, KFK_VTYPE_PARTITION = 3, KFK_VTYPE_VALUE = 4, KFK_VTYPE_KEY = 5,
       KFK_VTYPE_MSGFLAGS = 7, KFK_VTYPE_HEADERS = 10 };
#define KFK_MSG_F_COPY        0x2
#define KFK_PURGE_F_QUEUE     0x1
#define KFK_PURGE_F_INFLIGHT  0x2
#define KFK_PARTITION_UA      ((int32_t)-1)
#define KFK_OFFSET_BEGINNING  (-2)
#define KFK_OFFSET_END        (-1)
#define KFK_OFFSET_STORED     (-1000)
#define KFK_OFFSET_INVALID    (-1001)

typedef struct rd_kafka_message_s {
    rd_kafka_resp_err_t err;
    rd_kafka_topic_t*   rkt;
    int32_t             partition;
    void*               payload;
    size_t              len;
    void*               key;
    size_t              key_len;
    int64_t             offset;
    void*               _private;
} rd_kafka_message_t;

typedef struct rd_kafka_topic_partition_s {
    char*               topic;
    int32_t             partition;
    int64_t             offset;
    void*               metadata;
    size_t              metadata_size;
    void*               opaque;
    rd_kafka_resp_err_t err;
    void*               _private;
} rd_kafka_topic_partition_t;

typedef struct rd_kafka_topic_partition_list_s {
    int                         cnt;
    int                         size;
    rd_kafka_topic_partition_t* elems;
} rd_kafka_topic_partition_list_t;

typedef struct rd_kafka_metadata_broker { int32_t id; char* host; int port; } rd_kafka_metadata_broker_t;
typedef struct rd_kafka_metadata_partition {
    int32_t id; rd_kafka_resp_err_t err; int32_t leader; int replica_cnt; int32_t* replicas; int isr_cnt; int32_t* isrs;
} rd_kafka_metadata_partition_t;
typedef struct rd_kafka_metadata_topic {
    char* topic; int partition_cnt; rd_kafka_metadata_partition_t* partitions; rd_kafka_resp_err_t err;
} rd_kafka_metadata_topic_t;
typedef struct rd_kafka_metadata {
    int broker_cnt; rd_kafka_metadata_broker_t* brokers; int topic_cnt; rd_kafka_metadata_topic_t* topics;
    int32_t orig_broker_id; char* orig_broker_name;
} rd_kafka_metadata_t;

typedef void (*kfk_error_fn)(rd_kafka_t* rk, int err, const char* reason, void* opaque);
typedef int  (*kfk_stats_fn)(rd_kafka_t* rk, char* json, size_t json_len, void* opaque);
typedef void (*kfk_dr_fn)(rd_kafka_t* rk, const rd_kafka_message_t* m, void* opaque);
typedef void (*kfk_offset_commit_fn)(rd_kafka_t* rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t* offsets, void* opaque);

/* the table, in KFK_SYMS[] order */
typedef struct {
    const char*          (*version_str)(void);
    const char*          (*err2str)(rd_kafka_resp_err_t);
    const char*          (*err2name)(rd_kafka_resp_err_t);
    rd_kafka_conf_t*     (*conf_new)(void);
    void                 (*conf_destroy)(rd_kafka_conf_t*);
    rd_kafka_conf_res_t  (*conf_set)(rd_kafka_conf_t*, const char*, const char*, char*, size_t);
    void                 (*conf_set_opaque)(rd_kafka_conf_t*, void*);
    void                 (*conf_set_error_cb)(rd_kafka_conf_t*, kfk_error_fn);
    void                 (*conf_set_stats_cb)(rd_kafka_conf_t*, kfk_stats_fn);
    void                 (*conf_set_dr_msg_cb)(rd_kafka_conf_t*, kfk_dr_fn);
    void                 (*conf_set_offset_commit_cb)(rd_kafka_conf_t*, kfk_offset_commit_fn);
    rd_kafka_t*          (*new)(rd_kafka_type_t, rd_kafka_conf_t*, char*, size_t);
    void                 (*destroy)(rd_kafka_t*);
    int                  (*poll)(rd_kafka_t*, int);
    rd_kafka_resp_err_t  (*poll_set_consumer)(rd_kafka_t*);
    rd_kafka_queue_t*    (*queue_get_consumer)(rd_kafka_t*);
    rd_kafka_queue_t*    (*queue_get_main)(rd_kafka_t*);
    void                 (*queue_io_event_enable)(rd_kafka_queue_t*, int, const void*, size_t);
    void                 (*queue_destroy)(rd_kafka_queue_t*);
    rd_kafka_message_t*  (*consumer_poll)(rd_kafka_t*, int);
    rd_kafka_resp_err_t  (*consumer_close)(rd_kafka_t*);
    void                 (*message_destroy)(rd_kafka_message_t*);
    const char*          (*message_errstr)(const rd_kafka_message_t*);
    int64_t              (*message_timestamp)(const rd_kafka_message_t*, rd_kafka_timestamp_type_t*);
    rd_kafka_resp_err_t  (*message_headers)(const rd_kafka_message_t*, rd_kafka_headers_t**);
    size_t               (*header_cnt)(const rd_kafka_headers_t*);
    rd_kafka_resp_err_t  (*header_get_all)(const rd_kafka_headers_t*, size_t, const char**, const void**, size_t*);
    const char*          (*topic_name)(const rd_kafka_topic_t*);
    rd_kafka_topic_t*    (*topic_new)(rd_kafka_t*, const char*, rd_kafka_topic_conf_t*);
    void                 (*topic_destroy)(rd_kafka_topic_t*);
    rd_kafka_resp_err_t  (*subscribe)(rd_kafka_t*, const rd_kafka_topic_partition_list_t*);
    rd_kafka_resp_err_t  (*unsubscribe)(rd_kafka_t*);
    rd_kafka_resp_err_t  (*assign)(rd_kafka_t*, const rd_kafka_topic_partition_list_t*);
    rd_kafka_resp_err_t  (*assignment)(rd_kafka_t*, rd_kafka_topic_partition_list_t**);
    rd_kafka_resp_err_t  (*commit)(rd_kafka_t*, const rd_kafka_topic_partition_list_t*, int);
    rd_kafka_resp_err_t  (*commit_queue)(rd_kafka_t*, const rd_kafka_topic_partition_list_t*, rd_kafka_queue_t*,
                                         kfk_offset_commit_fn, void*);
    rd_kafka_resp_err_t  (*committed)(rd_kafka_t*, rd_kafka_topic_partition_list_t*, int);
    rd_kafka_resp_err_t  (*position)(rd_kafka_t*, rd_kafka_topic_partition_list_t*);
    rd_kafka_resp_err_t  (*pause_partitions)(rd_kafka_t*, rd_kafka_topic_partition_list_t*);
    rd_kafka_resp_err_t  (*resume_partitions)(rd_kafka_t*, rd_kafka_topic_partition_list_t*);
    rd_kafka_error_t*    (*seek_partitions)(rd_kafka_t*, rd_kafka_topic_partition_list_t*, int);
    rd_kafka_resp_err_t  (*offsets_for_times)(rd_kafka_t*, rd_kafka_topic_partition_list_t*, int);
    rd_kafka_topic_partition_list_t* (*topic_partition_list_new)(int);
    void                 (*topic_partition_list_destroy)(rd_kafka_topic_partition_list_t*);
    rd_kafka_topic_partition_t* (*topic_partition_list_add)(rd_kafka_topic_partition_list_t*, const char*, int32_t);
    rd_kafka_topic_partition_t* (*topic_partition_list_find)(const rd_kafka_topic_partition_list_t*, const char*, int32_t);
    rd_kafka_resp_err_t  (*producev)(rd_kafka_t*, ...);
    rd_kafka_resp_err_t  (*flush)(rd_kafka_t*, int);
    rd_kafka_resp_err_t  (*purge)(rd_kafka_t*, int);
    const rd_kafka_conf_t* (*conf)(rd_kafka_t*);
    rd_kafka_conf_res_t  (*conf_get)(const rd_kafka_conf_t*, const char*, char*, size_t*);
    rd_kafka_resp_err_t  (*metadata)(rd_kafka_t*, int, rd_kafka_topic_t*, const rd_kafka_metadata_t**, int);
    void                 (*metadata_destroy)(const rd_kafka_metadata_t*);
    const char*          (*error_string)(const rd_kafka_error_t*);
    void                 (*error_destroy)(rd_kafka_error_t*);
} kfk_api_t;

static const char* const KFK_SYMS[] = {
    "rd_kafka_version_str", "rd_kafka_err2str", "rd_kafka_err2name", "rd_kafka_conf_new",
    "rd_kafka_conf_destroy", "rd_kafka_conf_set", "rd_kafka_conf_set_opaque", "rd_kafka_conf_set_error_cb",
    "rd_kafka_conf_set_stats_cb", "rd_kafka_conf_set_dr_msg_cb", "rd_kafka_conf_set_offset_commit_cb", "rd_kafka_new",
    "rd_kafka_destroy", "rd_kafka_poll", "rd_kafka_poll_set_consumer", "rd_kafka_queue_get_consumer", "rd_kafka_queue_get_main",
    "rd_kafka_queue_io_event_enable", "rd_kafka_queue_destroy", "rd_kafka_consumer_poll", "rd_kafka_consumer_close",
    "rd_kafka_message_destroy", "rd_kafka_message_errstr", "rd_kafka_message_timestamp", "rd_kafka_message_headers", "rd_kafka_header_cnt",
    "rd_kafka_header_get_all", "rd_kafka_topic_name", "rd_kafka_topic_new", "rd_kafka_topic_destroy", "rd_kafka_subscribe", "rd_kafka_unsubscribe", "rd_kafka_assign",
    "rd_kafka_assignment", "rd_kafka_commit", "rd_kafka_commit_queue", "rd_kafka_committed", "rd_kafka_position",
    "rd_kafka_pause_partitions", "rd_kafka_resume_partitions",
    "rd_kafka_seek_partitions", "rd_kafka_offsets_for_times", "rd_kafka_topic_partition_list_new",
    "rd_kafka_topic_partition_list_destroy", "rd_kafka_topic_partition_list_add", "rd_kafka_topic_partition_list_find",
    "rd_kafka_producev",
    "rd_kafka_flush", "rd_kafka_purge", "rd_kafka_conf", "rd_kafka_conf_get", "rd_kafka_metadata", "rd_kafka_metadata_destroy", "rd_kafka_error_string", "rd_kafka_error_destroy",
    NULL
};

#endif /* Q_KAFKA_API_H */
