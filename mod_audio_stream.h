#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>
#include <speex/speex_resampler.h>

#define MY_BUG_NAME "audio_stream"
#define MY_STREAM_CONTEXT "audio_stream_context"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)
#define MAX_METADATA_LEN (8192)

/* Cap on how long stream_session_cleanup() waits for write_frame_thread to exit
   (normally ~20 ms, one timer tick). */
#define WRITE_THREAD_EXIT_TIMEOUT_MS (2000)

#define EVENT_CONNECT           "mod_audio_stream::connect"
#define EVENT_DISCONNECT        "mod_audio_stream::disconnect"
#define EVENT_ERROR             "mod_audio_stream::error"
#define EVENT_JSON              "mod_audio_stream::json"
#define EVENT_PLAY              "mod_audio_stream::play"

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json);

struct private_data {
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID];
    SpeexResamplerState *read_resampler;
    SpeexResamplerState *write_resampler;
    responseHandler_t responseHandler;
    void *pAudioStreamer;
    char ws_uri[MAX_WS_URI];
    int sampling;
    int wsSampling;
    int channels;
    /* Plain ints, not bitfields: each is touched from more than one thread and
       bitfields would share a storage unit. Cross-thread access uses __atomic_*. */
    int audio_paused;
    int close_requested;
    int cleanup_started;
    switch_buffer_t *read_sbuffer;
    switch_buffer_t *write_sbuffer;
    switch_mutex_t *write_mutex;
    switch_thread_t *write_thread;
    /* Set by write_frame_thread on exit, read by cleanup; always under write_mutex. */
    int write_thread_done;
    int rtp_packets;
};

typedef struct private_data private_t;

typedef enum {
    STREAM_STATE_IDLE = 0,
    STREAM_STATE_STARTING,
    STREAM_STATE_ACTIVE,
    STREAM_STATE_PAUSED,
    STREAM_STATE_STOPPING
} stream_state_t;

typedef struct stream_context {
    switch_mutex_t *mutex;
    stream_state_t state;
    switch_media_bug_t *bug;
    int startup_failed;
} stream_context_t;

enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE
};

#endif //MOD_AUDIO_STREAM_H
