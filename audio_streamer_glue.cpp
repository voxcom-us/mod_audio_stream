#include <string>
#include <cstring>
#include "mod_audio_stream.h"
#include "WebSocketClient.h"
#include <switch_json.h>
#include <fstream>
#include <switch_buffer.h>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/

class AudioStreamer {
public:
    // Factory
    static std::shared_ptr<AudioStreamer> create(
        const char* uuid, const char* wsUri, const char* metadata, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile,
        const char* tls_certfile, bool tls_disable_hostname_validation) {

        std::shared_ptr<AudioStreamer> sp(new AudioStreamer(
            uuid, wsUri, metadata, callback, deflate, heart_beat,
            suppressLog, extra_headers, tls_cafile, tls_keyfile,
            tls_certfile, tls_disable_hostname_validation
        ));

        sp->bindCallbacks(std::weak_ptr<AudioStreamer>(sp));

        sp->client.connect();

        return sp;
    }

    ~AudioStreamer()= default;

    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        client.disconnect();
    }

    /* Barge-in flush: drop incoming raw audio until the server's next non-audio
       message, which marks the end of the interrupted response. See .docs/lifecycle.md. */
    void discardIncomingAudio() {
        m_discardAudio.store(true, std::memory_order_release);
    }

    bool isConnected() {
        return client.isConnected();
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if(!m_readyForAudio.load(std::memory_order_acquire)) return;
        if(!this->isConnected()) return;
        client.sendBinary(buffer, len);
    }

    void writeText(const char* text) {
        if(!this->isConnected()) return;
        client.sendMessage(text, strlen(text));
    }

    void deleteFiles() {
        std::vector<std::string> files;

        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            if (m_Files.empty())
                return;

            files.assign(m_Files.begin(), m_Files.end());
            m_Files.clear();
            m_playFile = 0;
        }

        for (const auto& fn : files) {
            ::remove(fn.c_str());
        }
    }

    void markCleanedUp() {
        m_readyForAudio.store(false, std::memory_order_release);
        m_cleanedUp.store(true, std::memory_order_release);
        client.setMessageCallback({});
        client.setOpenCallback({});
        client.setErrorCallback({});
        client.setCloseCallback({});
    }

    bool isCleanedUp() const {
        return m_cleanedUp.load(std::memory_order_acquire);
    }

private:
    // Ctor
    AudioStreamer(
        const char* uuid, const char* wsUri, const char* metadata, responseHandler_t callback, int deflate, int heart_beat,
        bool suppressLog, const char* extra_headers, const char* tls_cafile, const char* tls_keyfile,
        const char* tls_certfile, bool tls_disable_hostname_validation
    ) : m_sessionId(uuid), m_initialMetadata(metadata ? metadata : ""), m_notify(callback), m_suppress_log(suppressLog),
        m_extra_headers(extra_headers), m_playFile(0) {

        WebSocketHeaders hdrs;
        WebSocketTLSOptions tls;

        if (m_extra_headers) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *iterator = headers_json->child;
                while (iterator) {
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr) {
                        hdrs.set(iterator->string, iterator->valuestring);
                    }
                    iterator = iterator->next;
                }
                cJSON_Delete(headers_json);
            }
        }

        client.setUrl(wsUri);

        // Setup TLS options
        // NONE - disables validation
        // SYSTEM - uses the system CAs bundle
        if (tls_cafile) {
            tls.caFile = tls_cafile;
        }

        if (tls_keyfile) {
            tls.keyFile = tls_keyfile;
        }

        if (tls_certfile) {
            tls.certFile = tls_certfile;
        }

        tls.disableHostnameValidation = tls_disable_hostname_validation;
        client.setTLSOptions(tls);

        // Optional heart beat, sent every xx seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        if(heart_beat)
            client.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if(deflate)
            client.enableCompression(false);

        // Set extra headers if any
        if(!hdrs.empty())
            client.setHeaders(hdrs);
    }

    struct ProcessResult {
        switch_bool_t ok = SWITCH_FALSE;
        std::string rewrittenJsonData;
        std::vector<std::string> errors;
        bool isRawAudio = false;
        int sampleRate = 0;
        std::vector<uint8_t> rawAudio;
    };

    static inline void push_err(ProcessResult& out, const std::string& sid, const std::string& s) {
        out.errors.push_back("(" + sid + ") " + s);
    }

    void bindCallbacks(std::weak_ptr<AudioStreamer> wp) {
        client.setMessageCallback([wp](const std::string& message) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;
            self->eventCallback(MESSAGE, message.c_str());
        });

        client.setOpenCallback([wp]() {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "connected");
            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECT_SUCCESS, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setErrorCallback([wp](int code, const std::string& msg) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "error");
            cJSON* message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "error", msg.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECT_ERROR, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setCloseCallback([wp](int code, const std::string& reason) {
            auto self = wp.lock();
            if (!self) return;
            if (self->isCleanedUp()) return;

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "disconnected");
            cJSON* message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "reason", reason.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char* json_str = cJSON_PrintUnformatted(root);

            self->eventCallback(CONNECTION_DROPPED, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);
        });
    }

    inline void media_bug_close(switch_core_session_t *session)
    {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return;
        }

        auto *ctx = (stream_context_t *)switch_channel_get_private(channel, MY_STREAM_CONTEXT);
        if (!ctx) {
            return;
        }

        switch_media_bug_t *bug = nullptr;

        switch_mutex_lock(ctx->mutex);

        if (ctx->state == STREAM_STATE_STARTING && !ctx->bug) {
            ctx->startup_failed = 1;
            switch_mutex_unlock(ctx->mutex);
            return;
        }

        if (ctx->bug &&
            (ctx->state == STREAM_STATE_ACTIVE ||
            ctx->state == STREAM_STATE_PAUSED)) {

            bug = ctx->bug;

            auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);

            if (tech_pvt) {
                __atomic_store_n(&tech_pvt->close_requested, 1, __ATOMIC_RELAXED);
            }
        }

        switch_mutex_unlock(ctx->mutex);

        if (bug) {
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    inline void send_initial_metadata() {
        if (!m_initialMetadata.empty()) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                              "sending initial metadata %s\n", m_initialMetadata.c_str());

            writeText(m_initialMetadata.c_str());
        }
    }

    /* tech_pvt of the running stream, or nullptr. It is session-pool allocated, so the
       pointer outlives ctx->mutex; close_requested / cleanup_started say if it is usable. */
    private_t *lookup_tech_pvt(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) return nullptr;

        auto *ctx = (stream_context_t *)switch_channel_get_private(channel, MY_STREAM_CONTEXT);
        if (!ctx) return nullptr;

        private_t *tech_pvt = nullptr;
        switch_mutex_lock(ctx->mutex);
        if (ctx->bug && (ctx->state == STREAM_STATE_ACTIVE || ctx->state == STREAM_STATE_PAUSED)) {
            tech_pvt = (private_t *)switch_core_media_bug_get_user_data(ctx->bug);
        }
        switch_mutex_unlock(ctx->mutex);
        return tech_pvt;
    }

    void injectRawAudio(switch_core_session_t *session, const std::vector<uint8_t>& rawAudio, int sampleRate) {
        private_t *tech_pvt = lookup_tech_pvt(session);
        if (!tech_pvt || !tech_pvt->write_sbuffer) return;
        if (__atomic_load_n(&tech_pvt->close_requested, __ATOMIC_RELAXED)) return;

        const int outRate = tech_pvt->sampling;
        const int channels = tech_pvt->channels;
        const int inRate = sampleRate ? sampleRate : tech_pvt->wsSampling;

        if (rawAudio.empty() || channels <= 0) return;

        spx_uint32_t in_frames = (spx_uint32_t)(rawAudio.size() / (sizeof(spx_int16_t) * channels));
        if (in_frames == 0) return;

        spx_uint32_t max_out = (spx_uint32_t)((double)in_frames * outRate / inRate) + 1;
        std::vector<spx_int16_t> in_buf(in_frames * channels);
        std::vector<spx_int16_t> out_buf(max_out * channels);
        std::memcpy(in_buf.data(), rawAudio.data(), rawAudio.size());

        spx_uint32_t in_len = in_frames;
        spx_uint32_t out_len = max_out;

        if (inRate == outRate || !tech_pvt->write_resampler) {
            // no resample needed - copy through
            out_buf.assign(in_buf.begin(), in_buf.end());
            out_len = in_len;
        } else if (channels == 1) {
            speex_resampler_process_int(tech_pvt->write_resampler, 0,
                                        in_buf.data(), &in_len,
                                        out_buf.data(), &out_len);
        } else {
            speex_resampler_process_interleaved_int(tech_pvt->write_resampler,
                                                    in_buf.data(), &in_len,
                                                    out_buf.data(), &out_len);
        }

        const size_t bytes_out = (size_t)out_len * (size_t)channels * sizeof(spx_int16_t);

        if (switch_mutex_lock(tech_pvt->write_mutex) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "%s injectRawAudio: write mutex lock failed, dropping %zu bytes\n",
                              tech_pvt->sessionId, bytes_out);
            return;
        }

        size_t remaining = bytes_out;
        const uint8_t *ptr = reinterpret_cast<const uint8_t *>(out_buf.data());
        while (remaining > 0) {
            /* Never spin here once teardown starts: we hold the session read lock
               (from eventCallback), and nothing is draining write_sbuffer any more. */
            if (__atomic_load_n(&tech_pvt->close_requested, __ATOMIC_RELAXED) ||
                __atomic_load_n(&tech_pvt->cleanup_started, __ATOMIC_RELAXED)) {
                break;
            }
            switch_size_t free_space = switch_buffer_freespace(tech_pvt->write_sbuffer);
            if (free_space == 0) {
                switch_mutex_unlock(tech_pvt->write_mutex);
                switch_yield(10000);
                if (switch_mutex_lock(tech_pvt->write_mutex) != SWITCH_STATUS_SUCCESS) return;
                continue;
            }
            size_t chunk = std::min<size_t>(remaining, free_space);
            switch_buffer_write(tech_pvt->write_sbuffer, ptr, chunk);
            ptr += chunk;
            remaining -= chunk;
        }

        switch_mutex_unlock(tech_pvt->write_mutex);
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        std::string msg = message ? message : "";

        // processing without holding a session
        ProcessResult pr;
        if (event == MESSAGE) {
            pr = processMessage(msg);
            if (pr.ok == SWITCH_TRUE) {
                msg = pr.rewrittenJsonData; // overwrite only on success
            }
        }

        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if (!psession) {
            return;
        }

        for (const auto& e : pr.errors) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_ERROR, "%s\n", e.c_str());
        }

        switch (event) {
            case CONNECT_SUCCESS:
                send_initial_metadata();
                m_readyForAudio.store(true, std::memory_order_release);
                m_notify(psession, EVENT_CONNECT, msg.c_str());
                break;

            case CONNECTION_DROPPED:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                m_notify(psession, EVENT_DISCONNECT, msg.c_str());
                media_bug_close(psession);
                break;

            case CONNECT_ERROR:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                m_notify(psession, EVENT_ERROR, msg.c_str());
                media_bug_close(psession);
                break;

            case MESSAGE:
                if (pr.isRawAudio) {
                    if (m_discardAudio.load(std::memory_order_acquire)) {
                        // Barge-in flush active: silently drop audio that
                        // belonged to the interrupted response.
                        break;
                    }
                    /* Don't inject into a dying channel - nothing drains it. */
                    {
                        switch_channel_t *ch = switch_core_session_get_channel(psession);
                        if (ch && switch_channel_ready(ch)) {
                            injectRawAudio(psession, pr.rawAudio, pr.sampleRate);
                        }
                    }
                } else {
                    // Any non-audio message from the server means the old TTS
                    // response has ended; re-enable audio injection.
                    m_discardAudio.store(false, std::memory_order_release);
                    if (pr.ok == SWITCH_TRUE) {
                        m_notify(psession, EVENT_PLAY, msg.c_str());
                    } else {
                        // fall back to EVENT_JSON
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }
                }

                if (!m_suppress_log && !pr.isRawAudio) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                                    "response: %s\n", msg.c_str());
                }
                break;
        }

        switch_core_session_rwunlock(psession);
    }


    ProcessResult processMessage(const std::string& message) {
        ProcessResult out;

        if (isCleanedUp()) return out;

        // RAII
        using jsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        jsonPtr root(cJSON_Parse(message.c_str()), &cJSON_Delete);
        if (!root) return out;

        const char* jsonType = cJSON_GetObjectCstr(root.get(), "type");
        if (!jsonType || std::strcmp(jsonType, "streamAudio") != 0) {
            return out; // not ours
        }

        cJSON* jsonData = cJSON_GetObjectItem(root.get(), "data");
        if (!jsonData) {
            push_err(out, m_sessionId, "processMessage - no data in streamAudio");
            return out;
        }

        const char* jsAudioDataType = cJSON_GetObjectCstr(jsonData, "audioDataType");
        if (!jsAudioDataType) jsAudioDataType = "";

        jsonPtr jsonAudio(cJSON_DetachItemFromObject(jsonData, "audioData"), &cJSON_Delete);

        if (!jsonAudio) {
            push_err(out, m_sessionId, "processMessage - streamAudio missing 'audioData' field");
            return out;
        }

        if (!cJSON_IsString(jsonAudio.get()) || !jsonAudio->valuestring) {
            push_err(out, m_sessionId, "processMessage - 'audioData' is not a string (expected base64 string)");
            return out;
        }

        // sampleRate (only meaningful for raw)
        int sampleRate = 0;
        if (cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate")) {
            sampleRate = jsonSampleRate->valueint;
        }

        const bool isRaw = std::strcmp(jsAudioDataType, "raw") == 0;

        // map file type (raw is handled out-of-band via write buffer; see eventCallback)
        std::string fileType;
        if (isRaw) {
            switch (sampleRate) {
                case 8000:
                case 16000:
                case 24000:
                case 32000:
                case 48000:
                case 64000:
                    break;
                default:
                    push_err(out, m_sessionId, "processMessage - unsupported sample rate: " + std::to_string(sampleRate));
                    return out;
            }
        } else if (std::strcmp(jsAudioDataType, "wav") == 0)  fileType = ".wav";
        else if (std::strcmp(jsAudioDataType, "mp3") == 0)   fileType = ".mp3";
        else if (std::strcmp(jsAudioDataType, "ogg") == 0)   fileType = ".ogg";
        else if (std::strcmp(jsAudioDataType, "pcmu") == 0)  fileType = ".pcmu";
        else if (std::strcmp(jsAudioDataType, "pcma") == 0)  fileType = ".pcma";
        else {
            push_err(out, m_sessionId, "processMessage - unsupported audio type: " + std::string(jsAudioDataType));
            return out;
        }

        // base64 decode
        std::string decoded;
        try {
            decoded = base64_decode(jsonAudio->valuestring);
        } catch (const std::exception& e) {
            push_err(out, m_sessionId, "processMessage - base64 decode error: " + std::string(e.what()));
            return out;
        }

        if (isRaw) {
            out.isRawAudio = true;
            out.sampleRate = sampleRate;
            out.rawAudio.assign(
                reinterpret_cast<const uint8_t*>(decoded.data()),
                reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
            return out;
        }

        // reserve file index
        int idx = 0;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            idx = m_playFile++;
        }

        char filePath[256];
        switch_snprintf(filePath, sizeof(filePath), "%s%s%s_%d.tmp%s",
                        SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
                        m_sessionId.c_str(), idx, fileType.c_str());

        // write file
        {
            std::ofstream f(filePath, std::ios::binary);
            if (!f.is_open()) {
                push_err(out, m_sessionId, std::string("processMessage - failed to open file for write: ") + filePath);
                return out;
            }
            f.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
            if (!f.good()) {
                push_err(out, m_sessionId, std::string("processMessage - failed writing file: ") + filePath);
                return out;
            }
        }

        // track file for cleanup
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            m_Files.insert(filePath);
        }

        cJSON_AddItemToObject(jsonData, "file", cJSON_CreateString(filePath));

        // return rewritten jsonData as string
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        if (!jsonString) {
            push_err(out, m_sessionId, "processMessage - cJSON_PrintUnformatted failed");
            return out;
        }

        out.rewrittenJsonData.assign(jsonString);
        std::free(jsonString);
        out.ok = SWITCH_TRUE;
        return out;
    }

private:
    std::string m_sessionId;
    std::string m_initialMetadata;
    responseHandler_t m_notify;
    WebSocketClient client;
    bool m_suppress_log;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_cleanedUp{false};
    std::mutex m_stateMutex;
    std::atomic<bool> m_readyForAudio{false};
    // Barge-in: drop incoming raw audio until the server sends a non-audio
    // message (which signals the old TTS response has ended).
    std::atomic<bool> m_discardAudio{false};
};


namespace {

    /* Session-pool allocated. tech_pvt must be passed in, not looked up through the
       context - cleanup nulls ctx->bug before waiting. See .docs/lifecycle.md. */
    struct write_thread_args {
        switch_core_session_t *session;
        private_t *tech_pvt;
    };

    void *SWITCH_THREAD_FUNC write_frame_thread(switch_thread_t *thread, void *obj) {
        auto *args = (write_thread_args *)obj;
        switch_core_session_t *session = args->session;
        private_t *tech_pvt = args->tech_pvt;

        /* Must be the first declaration: it publishes write_thread_done on every
           return below, and (destroyed last) only after the timer and codec are gone. */
        struct done_guard {
            private_t *p;
            ~done_guard() {
                switch_mutex_lock(p->write_mutex);
                p->write_thread_done = 1;
                switch_mutex_unlock(p->write_mutex);
            }
        } guard{tech_pvt};

        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) return NULL;

        /* Cleanup can win the race against our first instruction. */
        if (__atomic_load_n(&tech_pvt->close_requested, __ATOMIC_RELAXED)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                              "write_frame_thread: close already requested, not starting\n");
            return NULL;
        }

        switch_timer_t timer = {0};
        switch_frame_t write_frame = {0};
        switch_codec_t write_codec = {0};
        switch_codec_t *read_codec;

        uint32_t sample_rate = tech_pvt->sampling;
        uint32_t channels = tech_pvt->channels;

        read_codec = switch_core_session_get_read_codec(session);
        if (!read_codec || !read_codec->implementation) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "write_frame_thread: no read codec available, shutting down\n");
            return NULL;
        }

        uint32_t interval = read_codec->implementation->microseconds_per_packet / 1000;
        uint32_t samples = switch_samples_per_packet(sample_rate, interval);
        uint32_t tsamples = read_codec->implementation->actual_samples_per_second;
        uint32_t bytes = samples * 2 * channels;

        if (switch_core_codec_init(&write_codec, "L16", NULL, NULL, sample_rate, interval, channels,
                                   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
                                   switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "write_frame_thread: Codec Init Failed. Cannot Start Write Thread\n");
            return NULL;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "Codec Activated L16@%uhz %u channels %dms\n", sample_rate, channels, interval);
        write_frame.codec = &write_codec;
        write_frame.data = switch_core_session_alloc(session, SWITCH_RECOMMENDED_BUFFER_SIZE);
        write_frame.channels = channels;
        write_frame.rate = sample_rate;
        write_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "started write frame thread with sample rate [%u] interval [%u] samples [%u] tsamples [%u] bytes [%u]\n",
                          sample_rate, interval, samples, tsamples, bytes);

        if (switch_core_timer_init(&timer, "soft", interval, tsamples, NULL) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Timer Setup Failed. Cannot Start Write Thread\n");
            switch_core_codec_destroy(&write_codec);
            return NULL;
        }

        while (!__atomic_load_n(&tech_pvt->close_requested, __ATOMIC_RELAXED) && switch_core_session_running(session)) {
            if (switch_mutex_trylock(tech_pvt->write_mutex) == SWITCH_STATUS_SUCCESS) {
                switch_size_t available = switch_buffer_inuse(tech_pvt->write_sbuffer);
                if (available >= bytes) {
                    write_frame.datalen = (uint32_t)switch_buffer_read(tech_pvt->write_sbuffer, write_frame.data, bytes);
                    write_frame.samples = write_frame.datalen / 2 / channels;
                    /* Required: writing to a dying channel blocks on the session I/O
                       lock that teardown holds, deadlocking cleanup. */
                    if (switch_channel_ready(channel)) {
                        switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0);
                    }
                }
                switch_mutex_unlock(tech_pvt->write_mutex);
            }
            switch_core_timer_next(&timer);
        }

        switch_core_timer_destroy(&timer);
        switch_core_codec_destroy(&write_codec);
        return NULL;
    }

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool suppressLog, int rtp_packets, const char* extra_headers,
                                     const char *tls_cafile, const char *tls_keyfile, const char *tls_certfile,
                                     bool tls_disable_hostname_validation)
    {
        int err; //speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI - 1);
        tech_pvt->sampling = sampling;
        tech_pvt->wsSampling = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        tech_pvt->audio_paused = 0;

        //size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PERIOD * BUFFERED_SEC);
        const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);

        auto sp = AudioStreamer::create(tech_pvt->sessionId, wsUri, metadata, responseHandler, deflate, heart_beat,
                                        suppressLog, extra_headers, tls_cafile, tls_keyfile,
                                        tls_certfile, tls_disable_hostname_validation);

        tech_pvt->pAudioStreamer = new std::shared_ptr<AudioStreamer>(sp);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);
        switch_mutex_init(&tech_pvt->write_mutex, SWITCH_MUTEX_NESTED, pool);

        if (switch_buffer_create(pool, &tech_pvt->read_sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "%s: Error creating read switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (switch_buffer_create(pool, &tech_pvt->write_sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "%s: Error creating write switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->read_resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing read resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
            tech_pvt->write_resampler = speex_resampler_init(channels, desiredSampling, sampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing write resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        }
        else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        if (tech_pvt->read_resampler) {
            speex_resampler_destroy(tech_pvt->read_resampler);
            tech_pvt->read_resampler = nullptr;
        }
        if (tech_pvt->write_resampler) {
            speex_resampler_destroy(tech_pvt->write_resampler);
            tech_pvt->write_resampler = nullptr;
        }
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
        if (tech_pvt->write_mutex) {
            switch_mutex_destroy(tech_pvt->write_mutex);
            tech_pvt->write_mutex = nullptr;
        }
        if (tech_pvt->read_sbuffer) {
            switch_buffer_destroy(&tech_pvt->read_sbuffer);
            tech_pvt->read_sbuffer = nullptr;
        }
        if (tech_pvt->write_sbuffer) {
            switch_buffer_destroy(&tech_pvt->write_sbuffer);
            tech_pvt->write_sbuffer = nullptr;
        }
    }

    /* shared_ptr copy of the running stream's AudioStreamer, or empty (reason logged). */
    std::shared_ptr<AudioStreamer> active_streamer(switch_core_session_t *session, const char *who) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *ctx = (stream_context_t*)switch_channel_get_private(channel, MY_STREAM_CONTEXT);
        std::shared_ptr<AudioStreamer> streamer;

        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s failed because no bug\n", who);
            return streamer;
        }

        switch_mutex_lock(ctx->mutex);
        auto *bug = ctx->bug;
        if (!bug || (ctx->state != STREAM_STATE_ACTIVE && ctx->state != STREAM_STATE_PAUSED)) {
            switch_mutex_unlock(ctx->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s failed because stream is not active\n", who);
            return streamer;
        }

        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
            switch_mutex_lock(tech_pvt->mutex);
            if (tech_pvt->pAudioStreamer) {
                auto sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
                if (sp_wrap && *sp_wrap) {
                    streamer = *sp_wrap; // copy shared_ptr
                }
            }
            switch_mutex_unlock(tech_pvt->mutex);
        }
        switch_mutex_unlock(ctx->mutex);

        return streamer;
    }

}

extern "C" {
    int validate_ws_uri(const char* url, char* wsUri) {
        const char* scheme = nullptr;
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            scheme = "ws";
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            scheme = "wss";
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(*hostEnd) && *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(*portStart)) {
                    return 0;
                }
                ++portStart;
            }
        }

        if (std::strlen(url) >= MAX_WS_URI) {
            return 0;
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        auto streamer = active_streamer(session, "stream_session_send_text");
        if (!streamer) return SWITCH_STATUS_FALSE;

        streamer->writeText(text);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *ctx = (stream_context_t*)switch_channel_get_private(channel, MY_STREAM_CONTEXT);
        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }

        switch_mutex_lock(ctx->mutex);
        const stream_state_t expected = pause ? STREAM_STATE_ACTIVE : STREAM_STATE_PAUSED;
        if (!ctx->bug || ctx->state != expected) {
            switch_mutex_unlock(ctx->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_pauseresume failed because stream state is invalid\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *bug = ctx->bug;
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) {
            switch_mutex_unlock(ctx->mutex);
            return SWITCH_STATUS_FALSE;
        }

        switch_core_media_bug_flush(bug);
        __atomic_store_n(&tech_pvt->audio_paused, pause ? 1 : 0, __ATOMIC_RELAXED);
        ctx->state = pause ? STREAM_STATE_PAUSED : STREAM_STATE_ACTIVE;
        switch_mutex_unlock(ctx->mutex);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_flush(switch_core_session_t *session) {
        auto streamer = active_streamer(session, "stream_session_flush");
        if (!streamer) return SWITCH_STATUS_FALSE;

        // Audio already decoded into write_sbuffer is at most one chunk and is
        // left to play out; only audio still arriving from the websocket is dropped.
        streamer->discardIncomingAudio();

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "stream_session_flush: incoming audio discarded until the next non-audio message\n");
        return SWITCH_STATUS_SUCCESS;
    }

    /* Teardown for a tech_pvt whose bug was never attached (or never published):
       no bug to remove, no write thread to wait for. */
    void stream_session_discard(void *userData) {
        auto *tech_pvt = static_cast<private_t*>(userData);
        if (!tech_pvt) return;

        std::shared_ptr<AudioStreamer>* sp_wrap = nullptr;
        std::shared_ptr<AudioStreamer> streamer;

        switch_mutex_lock(tech_pvt->mutex);
        if (!__atomic_load_n(&tech_pvt->cleanup_started, __ATOMIC_RELAXED)) {
            __atomic_store_n(&tech_pvt->cleanup_started, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&tech_pvt->close_requested, 1, __ATOMIC_RELAXED);
            sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
            tech_pvt->pAudioStreamer = nullptr;
            if (sp_wrap && *sp_wrap) streamer = *sp_wrap;
        }
        switch_mutex_unlock(tech_pvt->mutex);

        if (sp_wrap) delete sp_wrap;
        if (streamer) {
            streamer->markCleanedUp();
            streamer->disconnect();
        }
        destroy_tech_pvt(tech_pvt);
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        void **ppUserData)
    {
        int deflate = 0;
        int heart_beat = 0;
        bool suppressLog = false;
        const char* buffer_size;
        const char* extra_headers;
        int rtp_packets = 1; //20ms burst
        const char* tls_cafile = NULL;
        const char* tls_keyfile = NULL;
        const char* tls_certfile = NULL;
        bool tls_disable_hostname_validation = false;

        switch_channel_t *channel = switch_core_session_get_channel(session);

        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        tls_cafile = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
        tls_keyfile = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
        tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");

        if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION")) {
            tls_disable_hostname_validation = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value <= INT_MAX && value >= INT_MIN) {
                heart_beat = (int) value;
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if(bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if(bSize >= 20){
                rtp_packets = bSize/20;
            }
        }

        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");

        // allocate per-session tech_pvt
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }
        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, channels,
                                                        metadata, responseHandler, deflate, heart_beat, suppressLog, rtp_packets,
                                                        extra_headers, tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation)) {
            stream_session_discard(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_write_thread_init(switch_core_session_t *session, void *pUserData) {
        private_t *tech_pvt = (private_t *)pUserData;
        switch_memory_pool_t *pool = switch_core_session_get_pool(session);
        switch_threadattr_t *thd_attr = NULL;
        switch_status_t status;

        auto *args = (write_thread_args *)switch_core_session_alloc(session, sizeof(write_thread_args));
        if (!args) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) error allocating write thread args\n", tech_pvt->sessionId);
            tech_pvt->write_thread = nullptr;
            tech_pvt->write_thread_done = 1;
            return SWITCH_STATUS_FALSE;
        }
        args->session = session;
        args->tech_pvt = tech_pvt;

        switch_threadattr_create(&thd_attr, pool);
        /* Detached, NOT joinable: cleanup cannot join on the hangup path, and an
           unjoined joinable thread leaks its stack mapping. See .docs/lifecycle.md. */
        switch_threadattr_detach_set(thd_attr, 1);
        switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
        tech_pvt->write_thread_done = 0;

        status = switch_thread_create(&tech_pvt->write_thread, thd_attr, write_frame_thread, args, pool);
        if (status != SWITCH_STATUS_SUCCESS) {
            /* apr_thread_create() leaves the handle set on failure; clearing it keeps
               cleanup from waiting out the timeout for a thread that never ran. */
            tech_pvt->write_thread = nullptr;
            tech_pvt->write_thread_done = 1;
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) failed to create write frame thread (%d)\n", tech_pvt->sessionId, status);
            return SWITCH_STATUS_FALSE;
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) return SWITCH_TRUE;
        if (__atomic_load_n(&tech_pvt->audio_paused, __ATOMIC_RELAXED) ||
            __atomic_load_n(&tech_pvt->cleanup_started, __ATOMIC_RELAXED)) return SWITCH_TRUE;

        std::shared_ptr<AudioStreamer> streamer;
        std::vector<std::vector<uint8_t>> pending_send;

        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        if (!tech_pvt->pAudioStreamer) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto sp_ptr = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
        if (!sp_ptr || !(*sp_ptr)) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        streamer = *sp_ptr;

        auto *resampler = tech_pvt->read_resampler;
        const int channels = tech_pvt->channels;
        const int rtp_packets = tech_pvt->rtp_packets;

        if (nullptr == resampler) {

            uint8_t data_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data_buf;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if (!frame.datalen) {
                    continue;
                }

                if (rtp_packets == 1) {
                    pending_send.emplace_back((uint8_t*)frame.data, (uint8_t*)frame.data + frame.datalen);
                    continue;
                }

                size_t freespace = switch_buffer_freespace(tech_pvt->read_sbuffer);

                if (freespace >= frame.datalen) {
                    switch_buffer_write(tech_pvt->read_sbuffer, static_cast<uint8_t *>(frame.data), frame.datalen);
                }

                if (switch_buffer_freespace(tech_pvt->read_sbuffer) == 0) {
                    switch_size_t inuse = switch_buffer_inuse(tech_pvt->read_sbuffer);
                    if (inuse > 0) {
                        std::vector<uint8_t> tmp(inuse);
                        switch_buffer_read(tech_pvt->read_sbuffer, tmp.data(), inuse);
                        switch_buffer_zero(tech_pvt->read_sbuffer);
                        pending_send.emplace_back(std::move(tmp));
                    }
                }
            }

        } else {

            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if(!frame.datalen) {
                    continue;
                }

                const size_t freespace = switch_buffer_freespace(tech_pvt->read_sbuffer);
                spx_uint32_t in_len = frame.samples;
                spx_uint32_t out_len = (freespace / (tech_pvt->channels * sizeof(spx_int16_t)));

                if(out_len == 0) {
                    if(freespace == 0) {
                        switch_size_t inuse = switch_buffer_inuse(tech_pvt->read_sbuffer);
                        if (inuse > 0) {
                            std::vector<uint8_t> tmp(inuse);
                            switch_buffer_read(tech_pvt->read_sbuffer, tmp.data(), inuse);
                            switch_buffer_zero(tech_pvt->read_sbuffer);
                            pending_send.emplace_back(std::move(tmp));
                        }
                    }
                    continue;
                }

                std::vector<spx_int16_t> out;
                out.resize((size_t)out_len * (size_t)channels);

                if(channels == 1) {
                    speex_resampler_process_int(resampler,
                                    0,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    out.data(),
                                    &out_len);
                } else {
                    speex_resampler_process_interleaved_int(resampler,
                                    (const spx_int16_t *)frame.data,
                                    &in_len,
                                    out.data(),
                                    &out_len);
                }

                if(out_len > 0) {
                    const size_t bytes_written = (size_t)out_len * (size_t)channels * sizeof(spx_int16_t);

                    if (rtp_packets == 1) { //20ms packet
                        const uint8_t* p = (const uint8_t*)out.data();
                        pending_send.emplace_back(p, p + bytes_written);
                        continue;
                    }

                    if (bytes_written <= switch_buffer_freespace(tech_pvt->read_sbuffer)) {
                        switch_buffer_write(tech_pvt->read_sbuffer, (const uint8_t *)out.data(), bytes_written);
                    }
                }

                if (switch_buffer_freespace(tech_pvt->read_sbuffer) == 0) {
                    switch_size_t inuse = switch_buffer_inuse(tech_pvt->read_sbuffer);
                    if (inuse > 0) {
                        std::vector<uint8_t> tmp(inuse);
                        switch_buffer_read(tech_pvt->read_sbuffer, tmp.data(), inuse);
                        switch_buffer_zero(tech_pvt->read_sbuffer);
                        pending_send.emplace_back(std::move(tmp));
                    }
                }
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);

        if (!streamer || !streamer->isConnected()) return SWITCH_TRUE;

        for (auto &chunk : pending_send) {
            if (!chunk.empty()) {
                streamer->writeBinary(chunk.data(), chunk.size());
            }
        }

        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *ctx = (stream_context_t*)switch_channel_get_private(channel, MY_STREAM_CONTEXT);
        if (!ctx) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no context - websocket connection already closed\n");
            return SWITCH_STATUS_FALSE;
        }

        switch_mutex_lock(ctx->mutex);
        auto *bug = ctx->bug;
        if (bug) {
            ctx->state = STREAM_STATE_STOPPING;
            ctx->bug = nullptr;
        }
        switch_mutex_unlock(ctx->mutex);

        if(bug)
        {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            char sessionId[MAX_SESSION_ID];
            strcpy(sessionId, tech_pvt->sessionId);

            std::shared_ptr<AudioStreamer>* sp_wrap = nullptr;
            std::shared_ptr<AudioStreamer> streamer;
            switch_thread_t *write_thread = nullptr;
            int write_thread_exited = 1; /* no thread to wait for -> full cleanup */

            switch_mutex_lock(tech_pvt->mutex);

            if (__atomic_load_n(&tech_pvt->cleanup_started, __ATOMIC_RELAXED)) {
                switch_mutex_unlock(tech_pvt->mutex);
                return SWITCH_STATUS_SUCCESS;
            }

            __atomic_store_n(&tech_pvt->cleanup_started, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&tech_pvt->close_requested, 1, __ATOMIC_RELAXED);

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

            sp_wrap = static_cast<std::shared_ptr<AudioStreamer>*>(tech_pvt->pAudioStreamer);
            tech_pvt->pAudioStreamer = nullptr;

            if (sp_wrap && *sp_wrap) {
                streamer = *sp_wrap;
            }

            write_thread = tech_pvt->write_thread;
            tech_pvt->write_thread = nullptr;

            switch_mutex_unlock(tech_pvt->mutex);

            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }

            if (sp_wrap) {
                delete sp_wrap;
                sp_wrap = nullptr;
            }

            if(streamer) {
                streamer->deleteFiles();
                if (text) streamer->writeText(text);

                /* Nulls all callbacks, so no websocket event can reach session
                   context after this point. */
                streamer->markCleanedUp();

                if (!channelIsClosing) {
                    /* Not in the teardown path - safe to block on the close handshake. */
                    streamer->disconnect();
                } else {
                    /* disconnect() blocks on the close handshake and a dead backend can
                       stall it indefinitely, so hand it to a detached thread; the moved
                       shared_ptr keeps the AudioStreamer alive until it finishes.
                       The catch is load-bearing, not style: we unwind into a C frame
                       (switch_core_media_bug_close), so an escaping exception would
                       std::terminate all of FreeSWITCH. See .docs/lifecycle.md. */
                    try {
                        std::thread([s = std::move(streamer)]() mutable {
                            s->disconnect();
                        }).detach();
                    } catch (const std::exception &e) {
                        /* streamer died with the closure; no synchronous fallback -
                           that would block teardown, which is what we are avoiding. */
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                          "(%s) stream_session_cleanup: could not spawn disconnect "
                                          "thread (%s); closing without handshake\n", sessionId, e.what());
                    } catch (...) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                          "(%s) stream_session_cleanup: could not spawn disconnect "
                                          "thread; closing without handshake\n", sessionId);
                    }
                }
            }

            if (write_thread) {
                if (!channelIsClosing) {
                    /* destroy_tech_pvt() below frees write_mutex and write_sbuffer, so
                       the thread must be out of its loop first. It is detached, so no
                       join - wait on write_thread_done (normally ~20 ms). */
                    int waited_ms = 0;
                    for (;;) {
                        switch_mutex_lock(tech_pvt->write_mutex);
                        write_thread_exited = tech_pvt->write_thread_done;
                        switch_mutex_unlock(tech_pvt->write_mutex);
                        if (write_thread_exited || waited_ms >= WRITE_THREAD_EXIT_TIMEOUT_MS) break;
                        switch_yield(5000); /* 5 ms */
                        waited_ms += 5;
                    }
                    if (!write_thread_exited) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                          "(%s) stream_session_cleanup: write thread did not report exit within "
                                          "%d ms; skipping destroy_tech_pvt to avoid use-after-free\n",
                                          sessionId, WRITE_THREAD_EXIT_TIMEOUT_MS);
                    }
                } else {
                    /* Cannot wait here: we are inside SWITCH_ABC_TYPE_CLOSE and the
                       teardown holds the session I/O lock. The detached thread self-exits
                       within a timer tick and needs no reclaiming.
                       Best-effort only - nothing orders that exit against the session pool
                       being freed. See .docs/lifecycle.md. */
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                      "(%s) stream_session_cleanup: not waiting for write thread on channel close "
                                      "(close_requested set, detached thread will self-exit)\n", sessionId);
                }
            }

            if (!channelIsClosing && write_thread_exited) {
                destroy_tech_pvt(tech_pvt);
            } else {
                /* Write thread may still be live (hangup path, or the wait timed out).
                   Leave write_mutex/write_sbuffer to the session pool, but the speex
                   resamplers are malloc'd - the pool never reclaims them, and the write
                   thread never touches them, so free them here. */
                if (tech_pvt->read_resampler) {
                    speex_resampler_destroy(tech_pvt->read_resampler);
                    tech_pvt->read_resampler = nullptr;
                }
                if (tech_pvt->write_resampler) {
                    speex_resampler_destroy(tech_pvt->write_resampler);
                    tech_pvt->write_resampler = nullptr;
                }
            }

            if (!channelIsClosing) {
                switch_mutex_lock(ctx->mutex);
                ctx->state = STREAM_STATE_IDLE;
                switch_mutex_unlock(ctx->mutex);
            }

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no bug - websocket connection already closed\n");
        return SWITCH_STATUS_FALSE;
    }
}
