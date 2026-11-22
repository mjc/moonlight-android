#include "Limelight-internal.h"
#ifdef __ANDROID__
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#endif

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static RTP_VIDEO_QUEUE rtpQueue;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
#ifdef __ANDROID__
static PLT_THREAD drainThread;
#endif
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
#define RTP_RECV_PACKETS_BUFFERED 2048

// Pull short packet bursts from Android's UDP receive queue with one syscall.
#ifdef __ANDROID__
#define VIDEO_PACKET_RECEIVE_BATCH_SIZE 64
#define VIDEO_RECV_RING_SLOTS 8192
#define VIDEO_RECV_RING_MASK (VIDEO_RECV_RING_SLOTS - 1)
// Wake VideoRecv once this many packets are buffered, or sooner on EOF, clock-sync, or idle.
#define VIDEO_RECV_WAKE_BATCH 32
// Coalesce active mid-frame bursts when plenty of ring space remains and the prior batch was small.
#define VIDEO_RECV_COALESCE_BATCH_THRESHOLD (VIDEO_PACKET_RECEIVE_BATCH_SIZE / 2)
#define VIDEO_RECV_COALESCE_MIN_FREE_SLOTS (VIDEO_PACKET_RECEIVE_BATCH_SIZE * 2)
#define VIDEO_RECV_COALESCE_NS (500L * 1000L)
#else
#define VIDEO_PACKET_RECEIVE_BATCH_SIZE 1
#endif

// Where receives can't be made non-blocking per call (Windows), how close to an unfinished
// frame's deadline the receive thread starts waking up for it even when no packets arrive (see
// LiSetPartialFrameDeadline()). Further out, it relies on the frame's own packets to wake it,
// which saves a select() for every packet. Elsewhere it always wakes for the deadline: packets
// often stall just before it, and the frame was then cut tens of ms late.
#define PARTIAL_DEADLINE_POLL_WINDOW_US 2000

// Set by LiSetPartialFrameDeadline() from the renderer, read by the receive thread
static PLT_MUTEX partialDeadlineLock;
static bool partialDeadlineEnabled;
static int64_t partialDeadlineOffsetUs;
static int partialDeadlineMinPercent;

// Clock sync with this fork's Sunshine (see SS_CLOCK_SYNC_REQUEST). Requests go out from the
// ping thread and replies come in on the receive thread, among the video packets.
#define CLOCK_SYNC_INTERVAL_MS 50

// The offset comes from the fastest of this many recent exchanges. Queueing and scheduling
// only ever make an exchange slower, so the fastest is the least disturbed. At 50 ms apart
// they span half a second, over which clocks 200 ppm apart drift 0.1 ms.
#define CLOCK_SYNC_SAMPLES 10

// Requests stop if none has been answered by then, as with any other host
#define CLOCK_SYNC_GIVE_UP_MS 3000

// Exchanges slower than this say nothing useful
#define CLOCK_SYNC_MAX_RTT_US 1000000

// Requests whose send times are kept to check replies against
#define CLOCK_SYNC_SENT_SLOTS 16

// RTP video timestamps are 32 bits at 90 kHz, so presentationTimeUs wraps after this long
#define RTP_TIMESTAMP_PERIOD_US (4294967296.0 * 1000.0 / 90.0)

typedef struct _CLOCK_SYNC_SAMPLE {
    int64_t rttUs;
    int64_t offsetUs; // The host's clock minus ours
    uint64_t hostUs; // When the host received the request
} CLOCK_SYNC_SAMPLE;

static PLT_MUTEX clockSyncLock;
static CLOCK_SYNC_SAMPLE clockSyncSamples[CLOCK_SYNC_SAMPLES];
static int clockSyncSampleCount;
static int clockSyncNextSample;
static bool clockSyncValid;
static int64_t clockSyncOffsetUs;
static uint64_t clockSyncHostUs;
static uint32_t clockSyncSentSequence[CLOCK_SYNC_SENT_SLOTS];
static uint64_t clockSyncSentUs[CLOCK_SYNC_SENT_SLOTS];

// FEC window, plus the 15-frame decode queue and the block still being assembled.
// On Android, size the pool for 348 Mbps + 20% FEC (~625 packets/frame); elsewhere
// a second window covers the queue and assembled block. A miss mallocs a buffer.
#ifdef __ANDROID__
#define VIDEO_PACKET_POOL_COUNT (RTP_RECV_PACKETS_BUFFERED * 3)
#else
#define VIDEO_PACKET_POOL_COUNT (RTP_RECV_PACKETS_BUFFERED * 2)
#endif

// The packet pointer is the start of the slot. The RTP queue entry stays at
// packetSize + MAX_RTP_HEADER_SIZE inside it, so a free slot keeps its freelist
// link in the first word instead of a header in front of the packet.
static char* videoPacketSlab;
static uintptr_t videoPacketSlabEnd;
static void* videoPacketFreeHead;
static int videoPacketBufferSize;
static PLT_MUTEX videoPacketPoolLock;

static int videoPacketSlotSize(void) {
    int size;

    size = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE + (int)sizeof(RTPV_QUEUE_ENTRY);
    if (size < (int)sizeof(void*)) {
        return 0;
    }
    // Each slot starts aligned, as a malloc buffer would. Spare bytes follow the queue entry.
    return (size + 15) & ~15;
}

static void createVideoPacketPool(void) {
    char* slab;
    int slotSize;
    int i;
    size_t bytes;
    void* head;

    slotSize = videoPacketSlotSize();
    videoPacketBufferSize = slotSize;
    if (slotSize <= 0) {
        return;
    }

    bytes = (size_t)VIDEO_PACKET_POOL_COUNT * (size_t)slotSize;
    slab = (char*)malloc(bytes);
    if (slab == NULL) {
        Limelog("Video packet pool: malloc() failed\n");
        return;
    }
    if (PltCreateMutex(&videoPacketPoolLock) != 0) {
        free(slab);
        Limelog("Video packet pool: PltCreateMutex() failed\n");
        return;
    }

    head = NULL;
    for (i = VIDEO_PACKET_POOL_COUNT - 1; i >= 0; i--) {
        char* slot = slab + (size_t)i * (size_t)slotSize;
        memcpy(slot, &head, sizeof(head));
        head = slot;
    }
    videoPacketFreeHead = head;
    videoPacketSlabEnd = (uintptr_t)slab + bytes;
    videoPacketSlab = slab;
}

static void destroyVideoPacketPool(void) {
    char* slab;

    // Clear the range before freeing the slab. videoPacketFree() after this
    // calls free() and does not write into the slab.
    slab = videoPacketSlab;
    videoPacketSlab = NULL;
    videoPacketSlabEnd = 0;
    videoPacketFreeHead = NULL;
    if (slab != NULL) {
        free(slab);
        PltDeleteMutex(&videoPacketPoolLock);
    }
}

static bool videoPacketIsPoolSlot(void* packet) {
    uintptr_t addr;
    uintptr_t base;

    if (videoPacketSlab == NULL || packet == NULL || videoPacketBufferSize <= 0) {
        return false;
    }
    addr = (uintptr_t)packet;
    base = (uintptr_t)videoPacketSlab;
    if (addr < base || addr >= videoPacketSlabEnd) {
        return false;
    }
    return (addr - base) % (uintptr_t)videoPacketBufferSize == 0;
}

void* videoPacketAlloc(void) {
    void* packet;

    if (videoPacketSlab != NULL) {
        PltLockMutex(&videoPacketPoolLock);
        packet = videoPacketFreeHead;
        if (packet != NULL) {
            memcpy(&videoPacketFreeHead, packet, sizeof(videoPacketFreeHead));
        }
        PltUnlockMutex(&videoPacketPoolLock);
        if (packet != NULL) {
            return packet;
        }
    }

    if (videoPacketBufferSize <= 0) {
        return NULL;
    }
    return malloc((size_t)videoPacketBufferSize);
}

static bool videoPacketAllocBatch(char** packets, int count) {
    int i;
    bool needed = false;

    if (videoPacketBufferSize <= 0) {
        return false;
    }

    for (i = 0; i < count; i++) {
        needed |= packets[i] == NULL;
    }
    if (!needed) {
        return true;
    }

    if (videoPacketSlab != NULL) {
        PltLockMutex(&videoPacketPoolLock);
        for (i = 0; i < count && videoPacketFreeHead != NULL; i++) {
            if (packets[i] == NULL) {
                packets[i] = (char*)videoPacketFreeHead;
                memcpy(&videoPacketFreeHead, packets[i], sizeof(videoPacketFreeHead));
            }
        }
        PltUnlockMutex(&videoPacketPoolLock);
    }

    for (i = 0; i < count; i++) {
        if (packets[i] == NULL) {
            packets[i] = (char*)malloc((size_t)videoPacketBufferSize);
            if (packets[i] == NULL) {
                return false;
            }
        }
    }
    return true;
}

void videoPacketFree(void* packet) {
    if (packet == NULL) {
        return;
    }

    // Pool memory is identified by the slab range. Any other pointer, including
    // an ordinary LENTRY and a fallback buffer, is freed.
    if (!videoPacketIsPoolSlot(packet)) {
        free(packet);
        return;
    }

    PltLockMutex(&videoPacketPoolLock);
    memcpy(packet, &videoPacketFreeHead, sizeof(videoPacketFreeHead));
    videoPacketFreeHead = packet;
    PltUnlockMutex(&videoPacketPoolLock);
}

void videoPacketFreeBatch(void** packets, int count) {
    int i;
    int poolPacketCount = 0;

    if (packets == NULL || count <= 0) {
        return;
    }

    // Keep ordinary allocations out of the pool lock. Queue purges commonly
    // return several pool packets at once, so link those with one lock hold.
    for (i = 0; i < count; i++) {
        if (packets[i] == NULL) {
            continue;
        }
        if (videoPacketIsPoolSlot(packets[i])) {
            packets[poolPacketCount++] = packets[i];
        }
        else {
            free(packets[i]);
        }
    }

    if (poolPacketCount == 0) {
        return;
    }

    PltLockMutex(&videoPacketPoolLock);
    for (i = 0; i < poolPacketCount; i++) {
        memcpy(packets[i], &videoPacketFreeHead, sizeof(videoPacketFreeHead));
        videoPacketFreeHead = packets[i];
    }
    PltUnlockMutex(&videoPacketPoolLock);
}

int videoReceiveRingPayloadBytes(int packetSize) {
#ifdef __ANDROID__
    uint64_t bytes;

    if (packetSize <= 0) {
        return 0;
    }

    bytes = (uint64_t)VIDEO_RECV_RING_SLOTS * (uint64_t)packetSize;
    if (bytes > (uint64_t)INT_MAX) {
        return INT_MAX;
    }
    return (int)bytes;
#else
    (void)packetSize;
    return 0;
#endif
}

#ifdef __ANDROID__
typedef struct _VIDEO_RECV_RING_SLOT {
    uint64_t receivedUs;
    int length;
} VIDEO_RECV_RING_SLOT, *PVIDEO_RECV_RING_SLOT;

typedef struct _VIDEO_RECV_RING {
    char* slab;
    char* dropBuffer;
    size_t slotStride;
    int receiveSize;
    int wakeupEventFd;
    _Alignas(64) atomic_uint_fast64_t writeIndex;
    _Alignas(64) atomic_uint_fast64_t readIndex;
    _Alignas(64) atomic_bool consumerWaiting;
    atomic_bool stopRequested;
    atomic_int socketError;
    atomic_uint_fast64_t droppedPackets;
} VIDEO_RECV_RING;

static VIDEO_RECV_RING videoRecvRing = {
    .wakeupEventFd = -1,
};

static inline PVIDEO_RECV_RING_SLOT videoRecvRingSlot(uint64_t index) {
    return (PVIDEO_RECV_RING_SLOT)(videoRecvRing.slab + (size_t)(index & VIDEO_RECV_RING_MASK) * videoRecvRing.slotStride);
}

static inline char* videoRecvRingSlotData(PVIDEO_RECV_RING_SLOT slot) {
    return (char*)slot + sizeof(VIDEO_RECV_RING_SLOT);
}

static void wakeVideoReceiveConsumer(void) {
    if (videoRecvRing.wakeupEventFd >= 0) {
        uint64_t one = 1;
        ssize_t ignored = write(videoRecvRing.wakeupEventFd, &one, sizeof(one));
        (void)ignored;
    }
}

static void drainVideoReceiveWakeup(void) {
    if (videoRecvRing.wakeupEventFd >= 0) {
        uint64_t val;
        ssize_t ignored = read(videoRecvRing.wakeupEventFd, &val, sizeof(val));
        (void)ignored;
    }
}

static void signalVideoReceiveRingError(int err) {
    atomic_store_explicit(&videoRecvRing.socketError, err != 0 ? err : -1, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&videoRecvRing.consumerWaiting, memory_order_relaxed)) {
        wakeVideoReceiveConsumer();
    }
}

static void requestVideoReceiveRingStop(void) {
    atomic_store_explicit(&videoRecvRing.stopRequested, true, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    wakeVideoReceiveConsumer();
}

static void destroyVideoReceiveRing(void) {
    if (videoRecvRing.wakeupEventFd >= 0) {
        close(videoRecvRing.wakeupEventFd);
        videoRecvRing.wakeupEventFd = -1;
    }
    free(videoRecvRing.slab);
    videoRecvRing.slab = NULL;
    free(videoRecvRing.dropBuffer);
    videoRecvRing.dropBuffer = NULL;
    videoRecvRing.slotStride = 0;
    videoRecvRing.receiveSize = 0;
}

static int createVideoReceiveRing(int receiveSize) {
    size_t stride;
    size_t slabBytes;
    int fd;
    char* slab;
    char* dropBuffer;

    destroyVideoReceiveRing();

    if (receiveSize <= 0) {
        return -1;
    }

    // Align each slot to 16 bytes so headers and packet payloads stay naturally aligned.
    stride = (sizeof(VIDEO_RECV_RING_SLOT) + (size_t)receiveSize + 15u) & ~(size_t)15u;
    slabBytes = (size_t)VIDEO_RECV_RING_SLOTS * stride;

    slab = (char*)malloc(slabBytes);
    if (slab == NULL) {
        Limelog("Video receive ring: malloc(%zu) failed\n", slabBytes);
        return -1;
    }

    dropBuffer = (char*)malloc((size_t)receiveSize);
    if (dropBuffer == NULL) {
        free(slab);
        Limelog("Video receive ring: dropBuffer malloc(%d) failed\n", receiveSize);
        return -1;
    }

    fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        int err = errno != 0 ? errno : -1;
        free(dropBuffer);
        free(slab);
        Limelog("Video receive ring: eventfd() failed: %d\n", err);
        return err;
    }

    videoRecvRing.slab = slab;
    videoRecvRing.dropBuffer = dropBuffer;
    videoRecvRing.slotStride = stride;
    videoRecvRing.receiveSize = receiveSize;
    videoRecvRing.wakeupEventFd = fd;
    atomic_init(&videoRecvRing.writeIndex, 0);
    atomic_init(&videoRecvRing.readIndex, 0);
    atomic_init(&videoRecvRing.consumerWaiting, false);
    atomic_init(&videoRecvRing.stopRequested, false);
    atomic_init(&videoRecvRing.socketError, 0);
    atomic_init(&videoRecvRing.droppedPackets, 0);
    return 0;
}
#endif

// Initialize the video stream
void initializeVideoStream(void) {
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
    PltCreateMutex(&partialDeadlineLock);
    partialDeadlineEnabled = false;
    partialDeadlineOffsetUs = 0;
    partialDeadlineMinPercent = 0;
    PltCreateMutex(&clockSyncLock);
    clockSyncSampleCount = 0;
    clockSyncNextSample = 0;
    clockSyncValid = false;
    clockSyncOffsetUs = 0;
    clockSyncHostUs = 0;
    memset(clockSyncSentSequence, 0, sizeof(clockSyncSentSequence));
    memset(clockSyncSentUs, 0, sizeof(clockSyncSentUs));
    createVideoPacketPool();
#ifdef __ANDROID__
    videoRecvRing.wakeupEventFd = -1;
    videoRecvRing.slab = NULL;
    videoRecvRing.dropBuffer = NULL;
    videoRecvRing.slotStride = 0;
    videoRecvRing.receiveSize = 0;
    atomic_init(&videoRecvRing.writeIndex, 0);
    atomic_init(&videoRecvRing.readIndex, 0);
    atomic_init(&videoRecvRing.consumerWaiting, false);
    atomic_init(&videoRecvRing.stopRequested, false);
    atomic_init(&videoRecvRing.socketError, 0);
    atomic_init(&videoRecvRing.droppedPackets, 0);
#endif
}

// Clean up the video stream
void destroyVideoStream(void) {
#ifdef __ANDROID__
    destroyVideoReceiveRing();
#endif
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
    RtpvCleanupQueue(&rtpQueue);
    PltDeleteMutex(&partialDeadlineLock);
    PltDeleteMutex(&clockSyncLock);
    destroyVideoPacketPool();
}

static void sendClockSyncRequest(LC_SOCKADDR* saddr, uint32_t sequence) {
    SS_CLOCK_SYNC_REQUEST request;
    uint64_t nowUs;

    memcpy(request.payload, VideoPingPayload.payload, sizeof(request.payload));
    request.magic = LE32(SS_CLOCK_SYNC_REQUEST_MAGIC);
    request.sequence = LE32(sequence);

    nowUs = PltGetMicroseconds();
    request.clientSendUs = LE64(nowUs);

    PltLockMutex(&clockSyncLock);
    clockSyncSentSequence[sequence % CLOCK_SYNC_SENT_SLOTS] = sequence;
    clockSyncSentUs[sequence % CLOCK_SYNC_SENT_SLOTS] = nowUs;
    PltUnlockMutex(&clockSyncLock);

    sendto(rtpSocket, (char*)&request, sizeof(request), 0, (struct sockaddr*)saddr, AddrLen);
}

static bool isClockSyncAnswered(void) {
    bool valid;

    PltLockMutex(&clockSyncLock);
    valid = clockSyncValid;
    PltUnlockMutex(&clockSyncLock);

    return valid;
}

// receivedUs is when the reply came off the socket
static void handleClockSyncReply(PSS_CLOCK_SYNC_REPLY reply, uint64_t receivedUs) {
    uint32_t sequence = LE32(reply->sequence);
    uint64_t clientSendUs = LE64(reply->clientSendUs);
    uint64_t hostReceiveUs = LE64(reply->hostReceiveUs);
    uint64_t hostSendUs = LE64(reply->hostSendUs);
    int slot = sequence % CLOCK_SYNC_SENT_SLOTS;
    int64_t rttUs, offsetUs;
    bool first;
    int i;

    PltLockMutex(&clockSyncLock);

    // Only an answer to a request of ours, once. An off-path sender can't know the send
    // time it has to echo.
    if (clientSendUs == 0 || clockSyncSentSequence[slot] != sequence || clockSyncSentUs[slot] != clientSendUs) {
        PltUnlockMutex(&clockSyncLock);
        return;
    }
    clockSyncSentUs[slot] = 0;

    rttUs = ((int64_t)receivedUs - (int64_t)clientSendUs) - ((int64_t)hostSendUs - (int64_t)hostReceiveUs);
    if (rttUs < 0 || rttUs > CLOCK_SYNC_MAX_RTT_US || hostSendUs < hostReceiveUs) {
        PltUnlockMutex(&clockSyncLock);
        return;
    }

    // The midpoints of the two legs, assuming they take as long as each other
    offsetUs = (((int64_t)hostReceiveUs - (int64_t)clientSendUs) + ((int64_t)hostSendUs - (int64_t)receivedUs)) / 2;

    clockSyncSamples[clockSyncNextSample].rttUs = rttUs;
    clockSyncSamples[clockSyncNextSample].offsetUs = offsetUs;
    clockSyncSamples[clockSyncNextSample].hostUs = hostReceiveUs;
    clockSyncNextSample = (clockSyncNextSample + 1) % CLOCK_SYNC_SAMPLES;
    if (clockSyncSampleCount < CLOCK_SYNC_SAMPLES) {
        clockSyncSampleCount++;
    }

    {
        int best = 0;
        for (i = 1; i < clockSyncSampleCount; i++) {
            if (clockSyncSamples[i].rttUs < clockSyncSamples[best].rttUs) {
                best = i;
            }
        }
        clockSyncOffsetUs = clockSyncSamples[best].offsetUs;
        clockSyncHostUs = clockSyncSamples[best].hostUs;
    }

    first = !clockSyncValid;
    clockSyncValid = true;

    PltUnlockMutex(&clockSyncLock);

    if (first) {
        Limelog("Clock sync: host answered, round trip %d us\n", (int)rttUs);
    }
}

bool LiGetHostTimeOnLocalClock(uint64_t presentationTimeUs, uint64_t* localTimeUs) {
    int64_t offsetUs;
    uint64_t hostUs;
    double periods;
    int64_t wraps;
    int64_t hostTimeUs;

    PltLockMutex(&clockSyncLock);
    if (!clockSyncValid) {
        PltUnlockMutex(&clockSyncLock);
        return false;
    }
    offsetUs = clockSyncOffsetUs;
    hostUs = clockSyncHostUs;
    PltUnlockMutex(&clockSyncLock);

    // presentationTimeUs wraps with the RTP timestamp, while the host's sync times don't, so
    // take the wrap that puts the frame nearest the latest sync
    periods = ((double)hostUs - (double)presentationTimeUs) / RTP_TIMESTAMP_PERIOD_US;
    wraps = periods >= 0 ? (int64_t)(periods + 0.5) : -(int64_t)(0.5 - periods);
    hostTimeUs = (int64_t)presentationTimeUs + (int64_t)((double)wraps * RTP_TIMESTAMP_PERIOD_US);

    if (hostTimeUs - offsetUs < 0) {
        return false;
    }

    *localTimeUs = (uint64_t)(hostTimeUs - offsetUs);
    return true;
}

void LiSetPartialFrameDeadline(bool enabled, int64_t offsetUs, int minReceivedPercent) {
    PltLockMutex(&partialDeadlineLock);
    partialDeadlineEnabled = enabled;
    partialDeadlineOffsetUs = offsetUs;
    partialDeadlineMinPercent = minReceivedPercent;
    PltUnlockMutex(&partialDeadlineLock);
}

bool getPartialFrameDeadline(int64_t* offsetUs, int* minReceivedPercent) {
    bool enabled;

    PltLockMutex(&partialDeadlineLock);
    enabled = partialDeadlineEnabled;
    *offsetUs = partialDeadlineOffsetUs;
    *minReceivedPercent = partialDeadlineMinPercent;
    PltUnlockMutex(&partialDeadlineLock);

    return enabled;
}

// Cuts the current frame short if it has reached its deadline. Returns how long until the
// deadline of the frame still arriving, or -1 if there is none.
static int64_t checkPartialFrameDeadline(void) {
    uint64_t deadlineUs, nowUs;

    if (!RtpvGetPartialFrameDeadline(&rtpQueue, &deadlineUs)) {
        return -1;
    }

    nowUs = PltGetMicroseconds();
    if (nowUs < deadlineUs) {
        return (int64_t)(deadlineUs - nowUs);
    }

    RtpvCheckPartialFrameDeadline(&rtpQueue, nowUs);
    return -1;
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    bool pinged = false;
    uint64_t lastPingMs = 0;
    uint32_t clockSyncSequence = 0;
    uint64_t clockSyncStartMs = 0;
    bool clockSyncGaveUp = false;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        uint64_t nowMs = PltGetMillis();
        bool clockSyncing;

        if (!pinged || nowMs - lastPingMs >= 500) {
            if (VideoPingPayload.payload[0] != 0) {
                pingCount++;
                VideoPingPayload.sequenceNumber = BE32(pingCount);

                sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
            }
            else {
                sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
            }

            pinged = true;
            lastPingMs = nowMs;
        }

        // Clock sync starts once video arrives, by when the host knows where to answer.
        // Hosts other than this fork's Sunshine never answer, and then it stops.
        clockSyncing = IS_SUNSHINE() && VideoPingPayload.payload[0] != 0 && !clockSyncGaveUp;
        if (clockSyncing && receivedDataFromPeer) {
            if (clockSyncStartMs == 0) {
                clockSyncStartMs = nowMs;
            }

            if (!isClockSyncAnswered() && nowMs - clockSyncStartMs >= CLOCK_SYNC_GIVE_UP_MS) {
                Limelog("Clock sync: no answer from the host, so it is not supported\n");
                clockSyncGaveUp = true;
                clockSyncing = false;
            }
            else {
                sendClockSyncRequest(&saddr, ++clockSyncSequence);
            }
        }

        PltSleepMsInterruptible(&udpPingThread, clockSyncing ? CLOCK_SYNC_INTERVAL_MS : 500);
    }
}

#ifdef __ANDROID__
static void setVideoThreadPriority(void) {
    // Match the renderer's display-priority threads so high-bitrate UDP bursts don't
    // stall behind normal-priority work.
    if (setpriority(PRIO_PROCESS, 0, -8) != 0) {
        setpriority(PRIO_PROCESS, 0, -4);
    }
}

static bool videoDrainStopped(void) {
    return PltIsThreadInterrupted(&drainThread) ||
           atomic_load_explicit(&videoRecvRing.stopRequested, memory_order_acquire);
}

static bool videoReceiveStopped(void) {
    return PltIsThreadInterrupted(&receiveThread) ||
           atomic_load_explicit(&videoRecvRing.stopRequested, memory_order_acquire);
}

static inline bool isUnencryptedEofVideoPacket(const char* data, int len) {
    if (len < (int)sizeof(RTP_PACKET)) {
        return false;
    }

    const uint8_t* bytes = (const uint8_t*)data;
    int dataOffset = (int)sizeof(RTP_PACKET);
    if (bytes[offsetof(RTP_PACKET, header)] & FLAG_EXTENSION) {
        dataOffset += 4;
    }

    if (len < dataOffset + (int)sizeof(NV_VIDEO_PACKET)) {
        return false;
    }

    return (bytes[dataOffset + offsetof(NV_VIDEO_PACKET, flags)] & FLAG_EOF) != 0;
}

static void flushPendingVideoReceiveWakeup(uint64_t writeIdx) {
    uint64_t readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_acquire);
    if (writeIdx != readIdx) {
        // Pair with VideoRecv's consumerWaiting store + seq_cst fence so a deferred wake
        // cannot be lost before VideoDrain blocks again.
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&videoRecvRing.consumerWaiting, memory_order_relaxed)) {
            wakeVideoReceiveConsumer();
        }
    }
}

static void VideoDrainThreadProc(void* context) {
    struct mmsghdr messages[VIDEO_PACKET_RECEIVE_BATCH_SIZE] = {0};
    struct iovec vectors[VIDEO_PACKET_RECEIVE_BATCH_SIZE];
    const struct timespec coalesceTs = {
        .tv_sec = 0,
        .tv_nsec = VIDEO_RECV_COALESCE_NS,
    };
    bool encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    bool useSelect;
    bool coalesceNext = false;
    uint64_t lastDropLogMs = 0;

    setVideoThreadPriority();

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        useSelect = true;
    }
    else {
        useSelect = false;
    }

    for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
        vectors[i].iov_len = (size_t)videoRecvRing.receiveSize;
        messages[i].msg_hdr.msg_iov = &vectors[i];
        messages[i].msg_hdr.msg_iovlen = 1;
    }

    while (!videoDrainStopped()) {
        uint64_t writeIdx = atomic_load_explicit(&videoRecvRing.writeIndex, memory_order_relaxed);
        uint64_t readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_acquire);
        uint64_t used = writeIdx - readIdx;
        int freeSlots = (used >= VIDEO_RECV_RING_SLOTS) ? 0 : (int)(VIDEO_RECV_RING_SLOTS - used);

        if (coalesceNext && freeSlots >= VIDEO_RECV_COALESCE_MIN_FREE_SLOTS) {
            (void)clock_nanosleep(CLOCK_MONOTONIC, 0, &coalesceTs, NULL);
            if (videoDrainStopped()) {
                break;
            }
            // Re-read readIndex in case VideoRecv drained packets while VideoDrain slept.
            readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_acquire);
            used = writeIdx - readIdx;
            freeSlots = (used >= VIDEO_RECV_RING_SLOTS) ? 0 : (int)(VIDEO_RECV_RING_SLOTS - used);
        }
        else {
            coalesceNext = false;
        }

        int ringSlotsToFill = freeSlots < VIDEO_PACKET_RECEIVE_BATCH_SIZE ? freeSlots : VIDEO_PACKET_RECEIVE_BATCH_SIZE;

        for (int i = 0; i < ringSlotsToFill; i++) {
            PVIDEO_RECV_RING_SLOT slot = videoRecvRingSlot((writeIdx + (uint64_t)i) & VIDEO_RECV_RING_MASK);
            vectors[i].iov_base = videoRecvRingSlotData(slot);
            messages[i].msg_len = 0;
        }
        for (int i = ringSlotsToFill; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
            vectors[i].iov_base = videoRecvRing.dropBuffer;
            messages[i].msg_len = 0;
        }

        int receivedCount;
        if (coalesceNext) {
            receivedCount = recvmmsg(rtpSocket, messages, VIDEO_PACKET_RECEIVE_BATCH_SIZE, MSG_DONTWAIT, NULL);
        }
        else if (useSelect || freeSlots == 0) {
            struct pollfd pfd;
            int pollRet;

            pfd.fd = rtpSocket;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pollRet = pollSockets(&pfd, 1, UDP_RECV_POLL_TIMEOUT_MS);
            if (pollRet < 0) {
                if (LastSocketError() == EINTR) {
                    continue;
                }
                Limelog("Video Drain: pollSockets() failed: %d\n", (int)LastSocketError());
                signalVideoReceiveRingError(LastSocketFail());
                break;
            }
            if (pollRet == 0) {
                flushPendingVideoReceiveWakeup(writeIdx);
                continue;
            }

            if (freeSlots == 0) {
                // Re-check whether the consumer freed slots while we waited in poll().
                readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_acquire);
                used = writeIdx - readIdx;
                freeSlots = (used >= VIDEO_RECV_RING_SLOTS) ? 0 : (int)(VIDEO_RECV_RING_SLOTS - used);
                ringSlotsToFill = freeSlots < VIDEO_PACKET_RECEIVE_BATCH_SIZE ? freeSlots : VIDEO_PACKET_RECEIVE_BATCH_SIZE;
                for (int i = 0; i < ringSlotsToFill; i++) {
                    PVIDEO_RECV_RING_SLOT slot = videoRecvRingSlot((writeIdx + (uint64_t)i) & VIDEO_RECV_RING_MASK);
                    vectors[i].iov_base = videoRecvRingSlotData(slot);
                    messages[i].msg_len = 0;
                }
            }

            receivedCount = recvmmsg(rtpSocket, messages, VIDEO_PACKET_RECEIVE_BATCH_SIZE, MSG_DONTWAIT, NULL);
        }
        else {
            receivedCount = recvmmsg(rtpSocket, messages, VIDEO_PACKET_RECEIVE_BATCH_SIZE, MSG_WAITFORONE, NULL);
        }

        if (receivedCount < 0) {
            int sockErr = LastSocketError();
            if (sockErr == EWOULDBLOCK || sockErr == EAGAIN || sockErr == EINTR) {
                coalesceNext = false;
                flushPendingVideoReceiveWakeup(writeIdx);
                continue;
            }
            Limelog("Video Drain: recvmmsg() failed: %d\n", sockErr);
            signalVideoReceiveRingError(LastSocketFail());
            break;
        }
        if (receivedCount == 0) {
            coalesceNext = false;
            flushPendingVideoReceiveWakeup(writeIdx);
            continue;
        }

        int accepted = receivedCount <= ringSlotsToFill ? receivedCount : ringSlotsToFill;
        int dropped = receivedCount - accepted;

        if (accepted > 0) {
            bool sawClockSync = false;
            bool sawEof = false;

            for (int i = 0; i < accepted; i++) {
                PVIDEO_RECV_RING_SLOT slot = videoRecvRingSlot((writeIdx + (uint64_t)i) & VIDEO_RECV_RING_MASK);
                int len = (int)messages[i].msg_len;
                slot->length = len;
                if (len == sizeof(SS_CLOCK_SYNC_REPLY)) {
                    slot->receivedUs = PltGetMicroseconds();
                    sawClockSync = true;
                }
                else if (!encrypted && isUnencryptedEofVideoPacket(videoRecvRingSlotData(slot), len)) {
                    sawEof = true;
                }
            }

            uint64_t nextWrite = writeIdx + (uint64_t)accepted;
            atomic_store_explicit(&videoRecvRing.writeIndex, nextWrite, memory_order_release);

            readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_acquire);
            uint64_t buffered = nextWrite - readIdx;
            if (sawClockSync || sawEof || buffered >= VIDEO_RECV_WAKE_BATCH) {
                atomic_thread_fence(memory_order_seq_cst);
                if (atomic_load_explicit(&videoRecvRing.consumerWaiting, memory_order_relaxed)) {
                    wakeVideoReceiveConsumer();
                }
            }

            int remainingFree = (buffered >= VIDEO_RECV_RING_SLOTS) ? 0 : (int)(VIDEO_RECV_RING_SLOTS - buffered);
            coalesceNext = !sawClockSync && !sawEof &&
                           receivedCount < VIDEO_RECV_COALESCE_BATCH_THRESHOLD &&
                           remainingFree >= VIDEO_RECV_COALESCE_MIN_FREE_SLOTS;
        }
        else {
            coalesceNext = false;
            flushPendingVideoReceiveWakeup(writeIdx);
        }

        if (dropped > 0) {
            uint64_t totalDropped = atomic_fetch_add_explicit(&videoRecvRing.droppedPackets,
                                                              (uint64_t)dropped,
                                                              memory_order_relaxed) + (uint64_t)dropped;
            uint64_t nowMs = PltGetMillis();
            if (lastDropLogMs == 0 || nowMs - lastDropLogMs >= 1000) {
                Limelog("Video Drain: ring full, dropped %d packets (total dropped: %llu)\n",
                        dropped, (unsigned long long)totalDropped);
                lastDropLogMs = nowMs;
            }
        }
    }
}

static bool processAndroidRingBatch(uint64_t readIdx, int batchCount, char** buffers,
                                    int decryptedSize, int minSize, bool encrypted,
                                    int waitingForVideoMs, bool* fatalPacket) {
    int consumed = 0;

    *fatalPacket = false;
    for (int batchIndex = 0; batchIndex < batchCount; batchIndex++) {
        PVIDEO_RECV_RING_SLOT slot = videoRecvRingSlot((readIdx + (uint64_t)batchIndex) & VIDEO_RECV_RING_MASK);
        char* slotData = videoRecvRingSlotData(slot);
        int err = slot->length;
        char* buffer = buffers[batchIndex];
        PRTP_PACKET packet;
        int queueStatus;

        consumed++;

        // Keep deadline handling at packet granularity, even while draining a burst.
        checkPartialFrameDeadline();

        // A clock sync reply, which can't be a video packet (see SS_CLOCK_SYNC_REPLY)
        if (err == sizeof(SS_CLOCK_SYNC_REPLY)) {
            PSS_CLOCK_SYNC_REPLY reply = (PSS_CLOCK_SYNC_REPLY)slotData;

            if (LE32(reply->magic) == SS_CLOCK_SYNC_REPLY_MAGIC) {
                handleClockSyncReply(reply, slot->receivedUs);
                continue;
            }
        }

        // Every packet counts, including FEC, duplicates, and packets of frames that are
        // later dropped
        rtpQueue.stats.byteCountReceived += (uint32_t)err;
        rtpQueue.stats.packetCountReceived++;

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

            firstDataTimeMs = PltGetMillis();
        }

#ifndef LC_FUZZING
        if (!receivedFullFrame) {
            if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                Limelog("Terminating connection due to lack of a successful video frame\n");
                ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                *fatalPacket = true;
                break;
            }
        }
#endif

        if (err < minSize || err > videoRecvRing.receiveSize) {
            // Runt packet
            continue;
        }

        if (encrypted) {
            PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)slotData;

            // Discard frames older than the active queue frame before decrypting.
            if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
                continue;
            }

            if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                   (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                   encHeader->iv, sizeof(encHeader->iv),
                                   encHeader->tag, sizeof(encHeader->tag),
                                   ((unsigned char*)(encHeader + 1)), err - sizeof(ENC_VIDEO_HEADER),
                                   (unsigned char*)buffer, &err)) {
                Limelog("Failed to decrypt video packet!\n");
                continue;
            }
        }
        else {
            memcpy(buffer, slotData, (size_t)err);
        }

        // Convert fields to host byte-order
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = BE16(packet->sequenceNumber);
        packet->timestamp = BE32(packet->timestamp);
        packet->ssrc = BE32(packet->ssrc);

        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

        if (queueStatus == RTPF_RET_QUEUED) {
            // The queue owns the buffer
            buffers[batchIndex] = NULL;
        }
    }

    atomic_store_explicit(&videoRecvRing.readIndex, readIdx + (uint64_t)consumed, memory_order_release);
    return !(*fatalPacket);
}

// Receive thread proc (consumes from the dedicated VideoDrain SPSC ring)
static void VideoReceiveThreadProc(void* context) {
    int decryptedSize, minSize;
    char* buffers[VIDEO_PACKET_RECEIVE_BATCH_SIZE] = {0};
    int waitingForVideoMs = 0;
    bool encrypted;

    setVideoThreadPriority();

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + (encrypted ? sizeof(ENC_VIDEO_HEADER) : 0);

    while (!videoReceiveStopped()) {
        int64_t untilDeadlineUs = checkPartialFrameDeadline();
        uint64_t readIdx = atomic_load_explicit(&videoRecvRing.readIndex, memory_order_relaxed);
        uint64_t writeIdx = atomic_load_explicit(&videoRecvRing.writeIndex, memory_order_acquire);

        if (readIdx == writeIdx) {
            int sockErr = atomic_load_explicit(&videoRecvRing.socketError, memory_order_acquire);
            if (sockErr != 0) {
                ListenerCallbacks.connectionTerminated(sockErr);
                break;
            }

            int timeoutMs = untilDeadlineUs >= 0 ? (int)((untilDeadlineUs + 999) / 1000)
                                                 : UDP_RECV_POLL_TIMEOUT_MS;
            if (timeoutMs > UDP_RECV_POLL_TIMEOUT_MS) {
                timeoutMs = UDP_RECV_POLL_TIMEOUT_MS;
            }

            atomic_store_explicit(&videoRecvRing.consumerWaiting, true, memory_order_relaxed);
            atomic_thread_fence(memory_order_seq_cst);

            writeIdx = atomic_load_explicit(&videoRecvRing.writeIndex, memory_order_acquire);
            sockErr = atomic_load_explicit(&videoRecvRing.socketError, memory_order_acquire);
            if (readIdx == writeIdx && sockErr == 0 && !videoReceiveStopped()) {
                struct pollfd pfd;
                pfd.fd = videoRecvRing.wakeupEventFd;
                pfd.events = POLLIN;
                pfd.revents = 0;

                int pollRet = poll(&pfd, 1, timeoutMs);
                atomic_store_explicit(&videoRecvRing.consumerWaiting, false, memory_order_relaxed);

                if (pollRet > 0 && (pfd.revents & POLLIN)) {
                    drainVideoReceiveWakeup();
                }
                else if (pollRet < 0) {
                    if (errno != EINTR) {
                        Limelog("Video Receive: poll(eventfd) failed: %d\n", errno);
                        ListenerCallbacks.connectionTerminated(LastSocketFail());
                        break;
                    }
                }
                else if (pollRet == 0) {
                    if (untilDeadlineUs < 0 && !receivedDataFromPeer) {
                        waitingForVideoMs += timeoutMs;
                        if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                            Limelog("Terminating connection due to lack of video traffic\n");
                            ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                            break;
                        }
                    }
                }
                continue;
            }

            atomic_store_explicit(&videoRecvRing.consumerWaiting, false, memory_order_relaxed);
            if (videoReceiveStopped()) {
                break;
            }
            if (readIdx == writeIdx) {
                if (sockErr != 0) {
                    ListenerCallbacks.connectionTerminated(sockErr);
                    break;
                }
                continue;
            }
        }

        uint64_t avail = writeIdx - readIdx;
        int batchCount = avail > VIDEO_PACKET_RECEIVE_BATCH_SIZE ? VIDEO_PACKET_RECEIVE_BATCH_SIZE : (int)avail;

        if (!videoPacketAllocBatch(buffers, batchCount)) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            goto cleanup;
        }

        bool fatalPacket = false;
        if (!processAndroidRingBatch(readIdx, batchCount, buffers, decryptedSize, minSize,
                                     encrypted, waitingForVideoMs, &fatalPacket)) {
            break;
        }
    }

cleanup:
    for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
        if (buffers[i] != NULL) {
            videoPacketFree(buffers[i]);
        }
    }
}
#else
// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int receiveSize, decryptedSize, minSize;
    char* buffers[VIDEO_PACKET_RECEIVE_BATCH_SIZE] = {0};
    char* encryptedBuffers[VIDEO_PACKET_RECEIVE_BATCH_SIZE] = {0};
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
            encryptedBuffers[i] = (char*)malloc(receiveSize);
            if (encryptedBuffers[i] == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                for (int j = 0; j < i; j++) {
                    free(encryptedBuffers[j]);
                }
                ListenerCallbacks.connectionTerminated(-1);
                return;
            }
        }
    }

    waitingForVideoMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        PRTP_PACKET packet;

        if (!videoPacketAllocBatch(buffers, VIDEO_PACKET_RECEIVE_BATCH_SIZE)) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            goto cleanup;
        }

        // Cut the current frame short if it has reached its deadline, and wake up for the
        // deadline even if no more packets come. A packet already waiting is taken without
        // blocking, which costs no more than an ordinary receive; only when none is there does
        // the thread wait, in poll(), until the deadline. On Windows, where a receive can't be
        // made non-blocking per call, it waits in select() once the deadline is close; a recv()
        // with a short SO_RCVTIMEO there often went 10 ms without returning.
        int64_t untilDeadlineUs = checkPartialFrameDeadline();
        int receivedCount = 0;
        bool received = false;
        if (untilDeadlineUs >= 0) {
            struct pollfd pfd;
            int timeoutMs = (int)((untilDeadlineUs + 999) / 1000);
            if (timeoutMs > UDP_RECV_POLL_TIMEOUT_MS) {
                timeoutMs = UDP_RECV_POLL_TIMEOUT_MS;
            }

            pfd.fd = rtpSocket;
            pfd.events = POLLIN;
#ifdef MSG_DONTWAIT
            err = (int)recvfrom(rtpSocket, encrypted ? encryptedBuffers[0] : buffers[0], receiveSize, MSG_DONTWAIT, NULL, NULL);
            if (err >= 0) {
                received = true;
            }
            else if (LastSocketError() == EWOULDBLOCK || LastSocketError() == EAGAIN || LastSocketError() == EINTR) {
                if (pollSockets(&pfd, 1, timeoutMs) == 0) {
                    // Nothing arrived before the deadline
                    continue;
                }
            }
#else
            if (untilDeadlineUs <= PARTIAL_DEADLINE_POLL_WINDOW_US && pollSockets(&pfd, 1, timeoutMs) == 0) {
                // Nothing arrived before the deadline
                continue;
            }
#endif
        }

        if (!received) {
            err = recvUdpSocket(rtpSocket,
                                encrypted ? encryptedBuffers[0] : buffers[0],
                                receiveSize,
                                useSelect);
        }
        if (err < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if (err > 0) {
            receivedCount = 1;
        }
        else {
            if (!receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }

            // Receive timed out; try again
            continue;
        }

        if (receivedCount == 0) {
            if (untilDeadlineUs < 0 && !receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }
            continue;
        }

        bool fatalPacket = false;
        for (int batchIndex = 0; batchIndex < receivedCount; batchIndex++) {
            buffer = buffers[batchIndex];
            encryptedBuffer = encrypted ? encryptedBuffers[batchIndex] : NULL;
            // Keep deadline handling at packet granularity, even while draining a burst.
            checkPartialFrameDeadline();

            // A clock sync reply, which can't be a video packet (see SS_CLOCK_SYNC_REPLY)
            if (err == sizeof(SS_CLOCK_SYNC_REPLY)) {
                uint64_t receivedUs = PltGetMicroseconds();
                PSS_CLOCK_SYNC_REPLY reply = (PSS_CLOCK_SYNC_REPLY)(encrypted ? encryptedBuffer : buffer);

                if (LE32(reply->magic) == SS_CLOCK_SYNC_REPLY_MAGIC) {
                    handleClockSyncReply(reply, receivedUs);
                    continue;
                }
            }

            // Every packet counts, including FEC, duplicates, and packets of frames that are
            // later dropped
            rtpQueue.stats.byteCountReceived += (uint32_t)err;
            rtpQueue.stats.packetCountReceived++;

            if (!receivedDataFromPeer) {
                receivedDataFromPeer = true;
                Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

                firstDataTimeMs = PltGetMillis();
            }

#ifndef LC_FUZZING
            if (!receivedFullFrame) {
                if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of a successful video frame\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                    fatalPacket = true;
                    break;
                }
            }
#endif

            if (err < minSize) {
                // Runt packet
                continue;
            }

            // Decrypt the packet into the buffer if encryption is enabled
            if (encrypted) {
                PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)encryptedBuffer;

                // If this frame is below our current frame number, discard it before decryption
                // to save CPU cycles decrypting FEC shards for a frame we already reassembled.
                //
                // Since this is happening _before_ decryption, this packet is not trusted yet.
                // It's imperative that we do not mutate any state based on this packet until
                // after it has been decrypted successfully!
                //
                // It's possible for an attacker to inject a fake packet that has any value of
                // header fields they want, however this provides them no benefit because we will
                // simply drop said packet here (if it's below the current frame number) or it
                // will pass this check and be dropped during decryption (if contents is tampered)
                // or after decryption in the RTP queue (if it's a replay of a previous authentic
                // packet from the host).
                //
                // In short, an attacker spoofing this value via MITM or sending malicious values
                // impersonating the host from off-link doesn't gain them anything. If they have
                // a true MITM, they can DoS our connection by just dropping all our traffic, so
                // tampering with packets to fail this check doesn't accomplish anything they
                // couldn't already do. If they're not on-link, we just throw their malicious
                // traffic away (as mentioned in the paragraph above) and continue accepting
                // legitmate video traffic.
                if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
                    continue;
                }

                if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                       (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                       encHeader->iv, sizeof(encHeader->iv),
                                       encHeader->tag, sizeof(encHeader->tag),
                                       ((unsigned char*)(encHeader + 1)), err - sizeof(ENC_VIDEO_HEADER), // The ciphertext is after the header
                                       (unsigned char*)buffer, &err)) {
                    Limelog("Failed to decrypt video packet!\n");
                    continue;
                }
            }

            // Convert fields to host byte-order
            packet = (PRTP_PACKET)&buffer[0];
            packet->sequenceNumber = BE16(packet->sequenceNumber);
            packet->timestamp = BE32(packet->timestamp);
            packet->ssrc = BE32(packet->ssrc);

            queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

            if (queueStatus == RTPF_RET_QUEUED) {
                // The queue owns the buffer
                buffers[batchIndex] = NULL;
            }
        }
        if (fatalPacket) {
            break;
        }
    }

cleanup:
    for (int i = 0; i < VIDEO_PACKET_RECEIVE_BATCH_SIZE; i++) {
        if (buffers[i] != NULL) {
            videoPacketFree(buffers[i]);
        }
        free(encryptedBuffers[i]);
    }
}
#endif

void notifyKeyFrameReceived(void) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

static void cleanupFailedVideoStreamStart(bool hasRecv, bool hasDrain, bool hasDec) {
    VideoCallbacks.stop();
    if (hasRecv || hasDec) {
        stopVideoDepacketizer();
    }
#ifdef __ANDROID__
    if (hasDrain) {
        PltInterruptThread(&drainThread);
    }
    if (hasRecv || hasDrain) {
        requestVideoReceiveRingStop();
    }
#else
    (void)hasDrain;
#endif
    if (hasRecv) {
        PltInterruptThread(&receiveThread);
    }
    if (hasDec) {
        PltInterruptThread(&decoderThread);
    }
#ifdef __ANDROID__
    if (hasDrain) {
        PltJoinThread(&drainThread);
    }
#endif
    if (hasRecv) {
        PltJoinThread(&receiveThread);
    }
    if (hasDec) {
        PltJoinThread(&decoderThread);
    }
#ifdef __ANDROID__
    destroyVideoReceiveRing();
#endif
    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }
    VideoCallbacks.cleanup();
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();

    PltInterruptThread(&udpPingThread);
#ifdef __ANDROID__
    PltInterruptThread(&drainThread);
    requestVideoReceiveRingStop();
#endif
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
#ifdef __ANDROID__
    PltJoinThread(&drainThread);
#endif
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltJoinThread(&decoderThread);
    }

#ifdef __ANDROID__
    destroyVideoReceiveRing();
#endif

    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              RTP_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }

#ifdef __ANDROID__
    {
        int receiveSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE +
                          ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
        err = createVideoReceiveRing(receiveSize);
        if (err != 0) {
            closeSocket(rtpSocket);
            rtpSocket = INVALID_SOCKET;
            VideoCallbacks.cleanup();
            return err;
        }
    }
#endif

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        cleanupFailedVideoStreamStart(false, false, false);
        return err;
    }

#ifdef __ANDROID__
    err = PltCreateThread("VideoDrain", VideoDrainThreadProc, NULL, &drainThread);
    if (err != 0) {
        cleanupFailedVideoStreamStart(true, false, false);
        return err;
    }
#endif

    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            cleanupFailedVideoStreamStart(true, true, false);
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, AddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            err = LastSocketError();
            cleanupFailedVideoStreamStart(true, true,
                                          (VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0);
            return err;
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        cleanupFailedVideoStreamStart(true, true,
                                      (VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0);
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void) {
    return &rtpQueue.stats;
}
