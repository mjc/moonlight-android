/* Run from the repository root:
 * devenv shell -- gcc -O1 -g -std=gnu11 -fsanitize=address,undefined \
 *   -ffunction-sections -fdata-sections \
 *   -Iapp/src/main/jni/moonlight-core/moonlight-common-c/enet/include \
 *   app/src/main/jni/moonlight-core/tests/video_receive_handoff_test.c \
 *   -Wl,--gc-sections -pthread -o app/build/video_receive_handoff_test
 * devenv shell -- app/build/video_receive_handoff_test
 */
#define _GNU_SOURCE
#define __ANDROID__ 1
#define LC_DEBUG 1
#include "../moonlight-common-c/src/VideoStream.c"
#include <assert.h>

STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
uint32_t EncryptionFeaturesEnabled;
int PltCreateMutex(PLT_MUTEX* m) { return pthread_mutex_init(m, NULL); }
void PltDeleteMutex(PLT_MUTEX* m) { pthread_mutex_destroy(m); }
void PltLockMutex(PLT_MUTEX* m) { pthread_mutex_lock(m); }
void PltUnlockMutex(PLT_MUTEX* m) { pthread_mutex_unlock(m); }
uint64_t PltGetMicroseconds(void) { return 1000000; }
uint64_t PltGetMillis(void) { return 1000; }
bool RtpvGetPartialFrameDeadline(PRTP_VIDEO_QUEUE q, uint64_t* d) { return false; }
void RtpvCheckPartialFrameDeadline(PRTP_VIDEO_QUEUE q, uint64_t n) {}
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE q) { return 0; }
bool PltDecryptMessage(PPLT_CRYPTO_CONTEXT c, int a, int f, unsigned char* k, int kl,
 unsigned char* iv, int il, unsigned char* t, int tl, unsigned char* in, int len,
 unsigned char* out, int* ol) { return false; }
static char* received[VIDEO_PACKET_RECEIVE_BATCH_SIZE];
static int calls;
int RtpvAddPacket(PRTP_VIDEO_QUEUE q, PRTP_PACKET p, int len, PRTPV_QUEUE_ENTRY entry) {
    assert(len == 128);
    assert(p->sequenceNumber == 1234);
    assert(((unsigned char*)p)[64] == 0x5a);
    received[calls] = (char*)p;
    return (calls++ & 1) ? RTPF_RET_REJECTED : RTPF_RET_QUEUED;
}

int main(void) {
    StreamConfig.packetSize = 1392;
    int size = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    for (int encrypted = 0; encrypted < 2; encrypted++) {
        EncryptionFeaturesEnabled = encrypted ? SS_ENC_VIDEO : 0;
        createVideoPacketPool();
        assert(videoPacketSlab);
        assert(createVideoReceiveRing(size + (encrypted ? sizeof(ENC_VIDEO_HEADER) : 0)) == 0);
        for (int i = 0; i < VIDEO_RECV_RING_SLOTS; i++) {
            char* p = videoRecvRingSlotData(videoRecvRingSlot(i));
            assert(p && ((uintptr_t)p & 15u) == 0);
            assert(videoPacketIsPoolSlot(p) == !encrypted);
        }
        if (!encrypted) {
            char* buffers[VIDEO_PACKET_RECEIVE_BATCH_SIZE] = {0};
            receivedDataFromPeer = receivedFullFrame = true;
            for (uint64_t base = 0; base < VIDEO_RECV_RING_SLOTS * 3; base += VIDEO_PACKET_RECEIVE_BATCH_SIZE) {
                char* original[VIDEO_PACKET_RECEIVE_BATCH_SIZE];
                assert(videoPacketAllocBatch(buffers, VIDEO_PACKET_RECEIVE_BATCH_SIZE));
                for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
                    PVIDEO_RECV_RING_SLOT slot = videoRecvRingSlot(base + i);
                    original[i] = videoRecvRingSlotData(slot);
                    memset(original[i], 0x5a, 128);
                    ((PRTP_PACKET)original[i])->sequenceNumber = BE16(1234);
                    slot->length = 128;
                }
                calls = 0;
                bool fatal = false;
                assert(processAndroidRingBatch(base, VIDEO_PACKET_RECEIVE_BATCH_SIZE, buffers,
                    size, sizeof(RTP_PACKET), false, 0, &fatal));
                assert(!fatal && calls == VIDEO_PACKET_RECEIVE_BATCH_SIZE);
                assert(atomic_load(&videoRecvRing.readIndex) == base + VIDEO_PACKET_RECEIVE_BATCH_SIZE);
                for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
                    assert(received[i] == original[i]);
                    assert(videoRecvRingSlotData(videoRecvRingSlot(base + i)) != original[i]);
                    if (!(i & 1)) {
                        assert(!buffers[i]);
                        videoPacketFree(received[i]);
                    } else {
                        assert(buffers[i] == original[i]);
                    }
                }
            }
            for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) videoPacketFree(buffers[i]);
        }
        destroyVideoReceiveRing();
        size_t free_count = 0;
        for (void* p = videoPacketFreeHead; p; free_count++) {
            assert(free_count < VIDEO_PACKET_POOL_COUNT + VIDEO_RECV_RING_SLOTS);
            memcpy(&p, p, sizeof(p));
        }
        assert(free_count == VIDEO_PACKET_POOL_COUNT + (encrypted ? 0 : VIDEO_RECV_RING_SLOTS));
        destroyVideoPacketPool();
    }
    puts("Receive handoff: three ring wraps, accepted/rejected packets, encrypted storage, and pool teardown passed");
}
