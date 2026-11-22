/* Run from the repository root:
 * devenv shell -- gcc -O1 -g -std=gnu11 -fsanitize=address,undefined \
 *   -ffunction-sections -fdata-sections \
 *   -Iapp/src/main/jni/moonlight-core/moonlight-common-c/enet/include \
 *   app/src/main/jni/moonlight-core/tests/audio_negotiation_test.c \
 *   -Wl,--gc-sections -pthread -o app/build/audio_negotiation_test
 * devenv shell -- app/build/audio_negotiation_test
 */
#define _GNU_SOURCE
#define __ANDROID__ 1
#define LC_DEBUG 1

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../moonlight-common-c/src/RtspConnection.c"
#include "../moonlight-common-c/src/SdpGenerator.c"
#include "../moonlight-common-c/src/AudioStream.c"

char* RemoteAddrString = NULL;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen = sizeof(struct sockaddr_in);
int AppVersionQuad[4] = { 7, 1, 431, -1 };
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
int NegotiatedVideoFormat = VIDEO_FORMAT_H264;
int NegotiatedAudioConfiguration = 0;
volatile bool ConnectionInterrupted = false;
bool HighQualitySurroundSupported = false;
bool HighQualitySurroundEnabled = false;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
int AudioPacketDuration = 5;
bool AudioEncryptionEnabled = false;
bool ReferenceFrameInvalidationSupported = false;
uint16_t RtspPortNumber = 48010;
uint16_t ControlPortNumber = 47999;
uint16_t AudioPortNumber = 48000;
uint16_t VideoPortNumber = 47998;
SS_PING AudioPingPayload;
SS_PING VideoPingPayload;
uint32_t ControlConnectData = 0;
uint32_t SunshineFeatureFlags = 0;
uint32_t EncryptionFeaturesSupported = 0;
uint32_t EncryptionFeaturesRequested = 0;
uint32_t EncryptionFeaturesEnabled = 0;

static int test_socket_close_count = 0;
static int test_fail_thread_on_call = 0;
static int test_thread_create_calls = 0;
static int test_ping_thread_joined = 0;
static int test_recv_thread_joined = 0;
static int test_init_audio_config = 0;
static OPUS_MULTISTREAM_CONFIGURATION test_init_opus_config;
static int test_ar_cleanup_calls = 0;

int PltCreateMutex(PLT_MUTEX* m) { return pthread_mutex_init(m, NULL); }
void PltDeleteMutex(PLT_MUTEX* m) { pthread_mutex_destroy(m); }
void PltLockMutex(PLT_MUTEX* m) { pthread_mutex_lock(m); }
void PltUnlockMutex(PLT_MUTEX* m) { pthread_mutex_unlock(m); }
uint64_t PltGetMicroseconds(void) { return 1000000; }
uint64_t PltGetMillis(void) { return 1000; }
void PltSleepMs(int ms) { (void)ms; }
void PltSleepMsInterruptible(PLT_THREAD* thread, int ms) { (void)thread; (void)ms; }
int PltCreateThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread) {
    (void)entry;
    (void)context;
    test_thread_create_calls++;
    thread->cancelled = false;
    if (test_fail_thread_on_call > 0 && test_thread_create_calls == test_fail_thread_on_call) {
        thread->thread = 0;
        return -1;
    }
    if (strcmp(name, "AudioPing") == 0) {
        thread->thread = (pthread_t)1;
    } else if (strcmp(name, "AudioRecv") == 0) {
        thread->thread = (pthread_t)2;
    } else {
        thread->thread = (pthread_t)3;
    }
    return 0;
}
void PltInterruptThread(PLT_THREAD* thread) { thread->cancelled = true; }
void PltJoinThread(PLT_THREAD* thread) {
    if (thread->thread == (pthread_t)1) {
        test_ping_thread_joined++;
    } else if (thread->thread == (pthread_t)2) {
        test_recv_thread_joined++;
    }
}
bool PltIsThreadInterrupted(PLT_THREAD* thread) { return thread->cancelled; }
PPLT_CRYPTO_CONTEXT PltCreateCryptoContext(void) { return (PPLT_CRYPTO_CONTEXT)1; }
void PltDestroyCryptoContext(PPLT_CRYPTO_CONTEXT ctx) { (void)ctx; }
bool PltDecryptMessage(PPLT_CRYPTO_CONTEXT ctx, int alg, int flags, unsigned char* k, int kl,
                       unsigned char* iv, int il, unsigned char* t, int tl, unsigned char* in, int len,
                       unsigned char* out, int* ol) {
    (void)ctx; (void)alg; (void)flags; (void)k; (void)kl; (void)iv; (void)il;
    (void)t; (void)tl; (void)in; (void)len; (void)out; (void)ol;
    return false;
}
bool PltEncryptMessage(PPLT_CRYPTO_CONTEXT ctx, int alg, int flags, unsigned char* k, int kl,
                       unsigned char* t, int tl, unsigned char* iv, int il, unsigned char* in, int len,
                       unsigned char* out, int* ol) {
    (void)ctx; (void)alg; (void)flags; (void)k; (void)kl; (void)iv; (void)il;
    (void)t; (void)tl; (void)in; (void)len; (void)out; (void)ol;
    return false;
}

bool PltSafeStrcpy(char* dst, size_t size, const char* src) {
    size_t len = strlen(src);
    if (len >= size) return false;
    memcpy(dst, src, len + 1);
    return true;
}

SOCKET bindUdpSocket(int af, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int qos) {
    (void)af; (void)localAddr; (void)addrLen; (void)bufferSize; (void)qos;
    return (SOCKET)42;
}
void closeSocket(SOCKET s) {
    (void)s;
    test_socket_close_count++;
}
int setNonFatalRecvTimeoutMs(SOCKET s, int timeoutMs) { (void)s; (void)timeoutMs; return 0; }
int recvUdpSocket(SOCKET s, char* buf, int len, bool useSelect) { (void)s; (void)buf; (void)len; (void)useSelect; return 0; }
void addrToUrlSafeString(struct sockaddr_storage* addr, char* dst, size_t len) {
    (void)addr;
    snprintf(dst, len, "127.0.0.1");
}
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }
int udpReceivePayloadBytes(void) { return 0; }
int videoReceiveRingPayloadBytes(int packetSize) { (void)packetSize; return 0; }

void createRtspRequest(PRTSP_MESSAGE msg, char* encodedMessage, int flags, char* command, char* target,
                       char* protocol, int sequenceNumber, POPTION_ITEM optionsHead, char* payload, int payloadLength) {
    (void)msg; (void)encodedMessage; (void)flags; (void)command; (void)target; (void)protocol;
    (void)sequenceNumber; (void)optionsHead; (void)payload; (void)payloadLength;
}
void insertOption(POPTION_ITEM* head, POPTION_ITEM item) { (void)head; (void)item; }
char* getOptionContent(POPTION_ITEM head, char* option) { (void)head; (void)option; return NULL; }
void freeMessage(PRTSP_MESSAGE msg) { (void)msg; }
char* serializeRtspMessage(PRTSP_MESSAGE msg, int* len) { (void)msg; *len = 0; return NULL; }
int parseRtspMessage(PRTSP_MESSAGE msg, char* raw, int len) { (void)msg; (void)raw; (void)len; return 0; }
SOCKET connectTcpSocket(struct sockaddr_storage* dst, SOCKADDR_LEN len, unsigned short port, int timeout) {
    (void)dst; (void)len; (void)port; (void)timeout; return INVALID_SOCKET;
}
int sendMtuSafe(SOCKET s, char* buf, int len) { (void)s; (void)buf; (void)len; return -1; }
int sendMessage(SOCKET s, char* buf, int len) { (void)s; (void)buf; (void)len; return -1; }
int recvTcpSocket(SOCKET s, char* buf, int len) { (void)s; (void)buf; (void)len; return -1; }
int serviceEnetHost(ENetHost* client, ENetEvent* event, enet_uint32 timeoutMs) {
    (void)client; (void)event; (void)timeoutMs; return 0;
}

int LbqInitializeLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE q, int bound) { (void)q; (void)bound; return 0; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqDestroyLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE q) { (void)q; return NULL; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE q) { (void)q; return NULL; }
int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE q, void* data, PLINKED_BLOCKING_QUEUE_ENTRY entry) { (void)q; (void)data; (void)entry; return LBQ_SUCCESS; }
int LbqWaitForQueueElement(PLINKED_BLOCKING_QUEUE q, void** data) { (void)q; (void)data; return LBQ_INTERRUPTED; }
void LbqSignalQueueShutdown(PLINKED_BLOCKING_QUEUE q) { (void)q; }
int LbqGetItemCount(PLINKED_BLOCKING_QUEUE q) { (void)q; return 0; }
void RtpaInitializeQueue(PRTP_AUDIO_QUEUE q) { (void)q; }
void RtpaCleanupQueue(PRTP_AUDIO_QUEUE q) { (void)q; }
int RtpaAddPacket(PRTP_AUDIO_QUEUE q, PRTP_PACKET p, uint16_t len) { (void)q; (void)p; (void)len; return 0; }
PRTP_PACKET RtpaGetQueuedPacket(PRTP_AUDIO_QUEUE q, uint16_t offset, uint16_t* len) { (void)q; (void)offset; (void)len; return NULL; }

static int stub_ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
    (void)context; (void)arFlags;
    test_init_audio_config = audioConfiguration;
    test_init_opus_config = *opusConfig;
    return 0;
}
static void stub_ar_start(void) {}
static void stub_ar_stop(void) {}
static void stub_ar_cleanup(void) { test_ar_cleanup_calls++; }
static void stub_ar_decode(char* data, int len) { (void)data; (void)len; }

static void reset_test_state(void) {
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    StreamConfig.fps = 60;
    StreamConfig.bitrate = 20000;
    StreamConfig.packetSize = 1392;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    NegotiatedAudioConfiguration = 0;
    HighQualitySurroundSupported = false;
    HighQualitySurroundEnabled = false;
    AudioPacketDuration = 5;
    memset(&AudioCallbacks, 0, sizeof(AudioCallbacks));
    AudioCallbacks.init = stub_ar_init;
    AudioCallbacks.start = stub_ar_start;
    AudioCallbacks.stop = stub_ar_stop;
    AudioCallbacks.cleanup = stub_ar_cleanup;
    AudioCallbacks.decodeAndPlaySample = stub_ar_decode;
    AudioCallbacks.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    RemoteAddr.ss_family = AF_INET;
    test_socket_close_count = 0;
    test_fail_thread_on_call = 0;
    test_thread_create_calls = 0;
    test_ping_thread_joined = 0;
    test_recv_thread_joined = 0;
    test_init_audio_config = 0;
    test_ar_cleanup_calls = 0;
}

static void test_stereo_to_51_missing_surround_params_falls_back_to_stereo(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;

    RTSP_MESSAGE resp = { 0 };
    resp.payload = "v=0\r\nm=audio 48000 RTP/AVP 97\r\n";

    assert(parseOpusConfigurations(&resp) == 0);
    assert(NegotiatedAudioConfiguration == AUDIO_CONFIGURATION_STEREO);
    assert(NormalQualityOpusConfig.channelCount == 2);
    assert(NormalQualityOpusConfig.streams == 1);
    assert(NormalQualityOpusConfig.coupledStreams == 1);
    assert(!HighQualitySurroundSupported);

    int sdpLen = 0;
    char* sdp = getSdpPayloadForStreamConfig(14, &sdpLen);
    assert(sdp != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.numChannels:2 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.enable:0 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.AudioQuality:0 \r\n") != NULL);
    assert(!HighQualitySurroundEnabled);
    free(sdp);

    assert(startAudioStream(NULL, 0) == 0);
    assert(test_init_audio_config == AUDIO_CONFIGURATION_STEREO);
    assert(test_init_opus_config.channelCount == 2);
    assert(test_init_opus_config.streams == 1);
}

static void test_stereo_to_71_missing_surround_params_falls_back_to_stereo(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;

    RTSP_MESSAGE resp = { 0 };
    resp.payload = "v=0\r\nm=audio 48000 RTP/AVP 97\r\n";

    assert(parseOpusConfigurations(&resp) == 0);
    assert(NegotiatedAudioConfiguration == AUDIO_CONFIGURATION_STEREO);
    assert(NormalQualityOpusConfig.channelCount == 2);
    assert(NormalQualityOpusConfig.streams == 1);
}

static void test_stereo_request_ignores_host_surround_capabilities(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

    // Sunshine/PyroShine unconditionally advertise 5.1 and 7.1 surround-params in DESCRIBE.
    // Requesting stereo must never silently upgrade to 5.1 or 7.1.
    RTSP_MESSAGE resp = { 0 };
    resp.payload = "a=fmtp:97 surround-params=642014523\r\na=fmtp:97 surround-params=660012345\r\n"
                   "a=fmtp:97 surround-params=85301452367\r\n";

    assert(parseOpusConfigurations(&resp) == 0);
    assert(NegotiatedAudioConfiguration == AUDIO_CONFIGURATION_STEREO);
    assert(NormalQualityOpusConfig.channelCount == 2);
    assert(NormalQualityOpusConfig.streams == 1);
    assert(NormalQualityOpusConfig.coupledStreams == 1);
    assert(!HighQualitySurroundSupported);

    int sdpLen = 0;
    char* sdp = getSdpPayloadForStreamConfig(14, &sdpLen);
    assert(sdp != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.numChannels:2 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.enable:0 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.AudioQuality:0 \r\n") != NULL);
    assert(!HighQualitySurroundEnabled);
    free(sdp);

    assert(startAudioStream(NULL, 0) == 0);
    assert(test_init_audio_config == AUDIO_CONFIGURATION_STEREO);
    assert(test_init_opus_config.channelCount == 2);
    assert(test_init_opus_config.streams == 1);
}

static void test_71_falls_back_to_51_when_only_51_advertised(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;

    RTSP_MESSAGE resp = { 0 };
    resp.payload = "a=fmtp:97 surround-params=642014523\r\na=fmtp:97 surround-params=660012345\r\n";

    assert(parseOpusConfigurations(&resp) == 0);
    assert(NegotiatedAudioConfiguration == AUDIO_CONFIGURATION_51_SURROUND);
    assert(NormalQualityOpusConfig.channelCount == 6);
    assert(NormalQualityOpusConfig.streams == 4);
    assert(NormalQualityOpusConfig.coupledStreams == 2);
    assert(NormalQualityOpusConfig.mapping[3] == 3);
    assert(NormalQualityOpusConfig.mapping[4] == 5);
    assert(NormalQualityOpusConfig.mapping[5] == 2);
    assert(HighQualitySurroundSupported);

    int sdpLen = 0;
    char* sdp = getSdpPayloadForStreamConfig(14, &sdpLen);
    assert(sdp != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.numChannels:6 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.enable:1 \r\n") != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.AudioQuality:1 \r\n") != NULL);
    assert(HighQualitySurroundEnabled);
    free(sdp);

    assert(startAudioStream(NULL, 0) == 0);
    assert(test_init_audio_config == AUDIO_CONFIGURATION_51_SURROUND);
    assert(test_init_opus_config.channelCount == 6);
    assert(test_init_opus_config.streams == 6);
}

static void test_remote_surround_mtu_guard_disables_hq_surround(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
    StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;

    RTSP_MESSAGE resp = { 0 };
    resp.payload = "a=fmtp:97 surround-params=642014523\r\na=fmtp:97 surround-params=660012345\r\n";

    assert(parseOpusConfigurations(&resp) == 0);
    assert(HighQualitySurroundSupported);

    int sdpLen = 0;
    char* sdp = getSdpPayloadForStreamConfig(14, &sdpLen);
    assert(sdp != NULL);
    assert(strstr(sdp, "a=x-nv-audio.surround.AudioQuality:0 \r\n") != NULL);
    assert(!HighQualitySurroundEnabled);
    free(sdp);
}

static void test_audio_stream_error_path_no_double_close(void) {
    reset_test_state();
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    RTSP_MESSAGE resp = { 0 };
    resp.payload = "";
    assert(parseOpusConfigurations(&resp) == 0);

    assert(initializeAudioStream() == 0);
    assert(notifyAudioPortNegotiationComplete() == 0);
    assert(pingThreadStarted);
    assert(rtpSocket != INVALID_SOCKET);

    test_fail_thread_on_call = 3;
    assert(startAudioStream(NULL, 0) == -1);
    assert(test_recv_thread_joined == 1);
    assert(test_ping_thread_joined == 1);
    assert(!pingThreadStarted);
    assert(rtpSocket == INVALID_SOCKET);
    assert(test_socket_close_count == 1);
    assert(test_ar_cleanup_calls == 1);

    destroyAudioStream();
    assert(test_socket_close_count == 1);
}

int main(void) {
    test_stereo_to_51_missing_surround_params_falls_back_to_stereo();
    test_stereo_to_71_missing_surround_params_falls_back_to_stereo();
    test_stereo_request_ignores_host_surround_capabilities();
    test_71_falls_back_to_51_when_only_51_advertised();
    test_remote_surround_mtu_guard_disables_hq_surround();
    test_audio_stream_error_path_no_double_close();
    puts("Audio negotiation, remote surround MTU guard, and stream error cleanup tests passed");
    return 0;
}
