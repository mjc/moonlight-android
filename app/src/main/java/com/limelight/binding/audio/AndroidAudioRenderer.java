package com.limelight.binding.audio;

import android.annotation.TargetApi;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRouting;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import com.limelight.LimeLog;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.jni.MoonBridge;

import java.util.ArrayList;
import java.util.List;

public class AndroidAudioRenderer implements AudioRenderer {

    static final int MAX_RECOVERY_ATTEMPTS = 8;
    private static final long INITIAL_RECOVERY_BACKOFF_MS = 100;
    private static final long MAX_RECOVERY_BACKOFF_MS = 1600;
    private static final int Q15_INV_SQRT2 = 23170; // round(32768 / sqrt(2))

    static final class TrackAttempt {
        final boolean lowLatency;
        final int bufferSize;

        TrackAttempt(boolean lowLatency, int bufferSize) {
            this.lowLatency = lowLatency;
            this.bufferSize = bufferSize;
        }
    }

    interface RoutingCallback {
        void onDeviceChanged(int deviceId, int deviceType);
    }

    interface AudioTrackHandle {
        int write(short[] audioData, int offsetInShorts, int sizeInShorts);
        int getAudioSessionId();
        void pause();
        void flush();
        void release();
        void attachRoutingCallback(RoutingCallback callback);
        void detachRoutingCallback();
    }

    interface AudioTrackBackend {
        int getMinBufferSize(int sampleRate, int channelMask);
        int getNativeOutputSampleRate();
        long elapsedRealtimeMs();
        int getPendingAudioDurationMs();
        AudioTrackHandle createAndStartTrack(int channelMask, int sampleRate, int bufferSize, boolean lowLatency) throws Exception;
    }

    private final Context context;
    private final boolean enableAudioFx;
    private final AudioTrackBackend backend;
    private final Object trackLock = new Object();

    private AudioTrackHandle track;
    private MoonBridge.AudioConfiguration requestedAudioConfig;
    private int sampleRate;
    private int samplesPerFrame;
    private int inputChannelCount;
    private int outputChannelCount;
    private int audioSessionId;
    private boolean audioFxSessionOpen;
    private boolean started;
    private volatile boolean stopped;
    private volatile boolean routingChanged;
    private int lastRoutedDeviceId;
    private int recoveryAttempts;
    private long nextRecoveryTimeMs;
    private short[] downmixBuffer;

    public AndroidAudioRenderer(Context context, boolean enableAudioFx) {
        this(context, enableAudioFx, null);
    }

    AndroidAudioRenderer(Context context, boolean enableAudioFx, AudioTrackBackend backend) {
        this.context = context;
        this.enableAudioFx = enableAudioFx;
        this.backend = backend != null ? backend : new DefaultAudioTrackBackend();
    }

    private final class DefaultAudioTrackHandle implements AudioTrackHandle {
        private final AudioTrack audioTrack;
        private Object routingListener;

        DefaultAudioTrackHandle(AudioTrack audioTrack) {
            this.audioTrack = audioTrack;
        }

        @Override
        public int write(short[] audioData, int offsetInShorts, int sizeInShorts) {
            return audioTrack.write(audioData, offsetInShorts, sizeInShorts);
        }

        @Override
        public int getAudioSessionId() {
            return audioTrack.getAudioSessionId();
        }

        @Override
        public void pause() {
            audioTrack.pause();
        }

        @Override
        public void flush() {
            audioTrack.flush();
        }

        @Override
        public void release() {
            detachRoutingCallback();
            audioTrack.release();
        }

        @Override
        public void attachRoutingCallback(RoutingCallback callback) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && context != null) {
                try {
                    new RoutingHelper(audioTrack, callback).attach(this);
                } catch (Exception e) {
                    LimeLog.warning("Failed to attach audio routing listener: " + e.getMessage());
                }
            }
        }

        @Override
        public void detachRoutingCallback() {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && routingListener != null) {
                new RoutingHelper(audioTrack, null).detach(this);
            }
        }
    }

    @TargetApi(Build.VERSION_CODES.N)
    private static final class RoutingHelper {
        private final AudioTrack audioTrack;
        private final RoutingCallback callback;

        RoutingHelper(AudioTrack audioTrack, RoutingCallback callback) {
            this.audioTrack = audioTrack;
            this.callback = callback;
        }

        void attach(DefaultAudioTrackHandle handle) {
            AudioDeviceInfo initialDevice = audioTrack.getRoutedDevice();
            if (initialDevice != null && initialDevice.getId() != 0) {
                callback.onDeviceChanged(initialDevice.getId(), initialDevice.getType());
            }

            AudioRouting.OnRoutingChangedListener listener = new AudioRouting.OnRoutingChangedListener() {
                @Override
                public void onRoutingChanged(AudioRouting router) {
                    AudioDeviceInfo device = router.getRoutedDevice();
                    if (device != null && device.getId() != 0) {
                        callback.onDeviceChanged(device.getId(), device.getType());
                    }
                }
            };
            audioTrack.addOnRoutingChangedListener(listener, new Handler(Looper.getMainLooper()));
            handle.routingListener = listener;
        }

        void detach(DefaultAudioTrackHandle handle) {
            if (handle.routingListener instanceof AudioRouting.OnRoutingChangedListener) {
                try {
                    audioTrack.removeOnRoutingChangedListener((AudioRouting.OnRoutingChangedListener) handle.routingListener);
                } catch (Exception ignored) {}
                handle.routingListener = null;
            }
        }
    }

    private final class DefaultAudioTrackBackend implements AudioTrackBackend {
        @Override
        public int getMinBufferSize(int sampleRate, int channelMask) {
            return AudioTrack.getMinBufferSize(sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
        }

        @Override
        public int getNativeOutputSampleRate() {
            return AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC);
        }

        @Override
        public long elapsedRealtimeMs() {
            return SystemClock.elapsedRealtime();
        }

        @Override
        public int getPendingAudioDurationMs() {
            return MoonBridge.getPendingAudioDuration();
        }

        @Override
        public AudioTrackHandle createAndStartTrack(int channelMask, int sampleRate, int bufferSize, boolean lowLatency) {
            AudioTrack candidate = createAudioTrack(channelMask, sampleRate, bufferSize, lowLatency);
            if (candidate.getState() != AudioTrack.STATE_INITIALIZED) {
                candidate.release();
                throw new IllegalStateException("AudioTrack failed to initialize (state=" + candidate.getState() + ")");
            }
            candidate.play();
            return new DefaultAudioTrackHandle(candidate);
        }
    }

    static int channelMaskForCount(int channelCount) {
        switch (channelCount) {
            case 2:
                return AudioFormat.CHANNEL_OUT_STEREO;
            case 4:
                return AudioFormat.CHANNEL_OUT_QUAD;
            case 6:
                return AudioFormat.CHANNEL_OUT_5POINT1;
            case 8:
                // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND isn't available until Android 6.0,
                // yet the CHANNEL_OUT_SIDE_LEFT and CHANNEL_OUT_SIDE_RIGHT constants were added
                // in 5.0, so just hardcode the constant so we can work on Lollipop.
                return 0x000018fc; // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
            default:
                return 0;
        }
    }

    static int roundUpToFrame(int sizeBytes, int bytesPerFrame) {
        if (bytesPerFrame <= 0) {
            return sizeBytes;
        }
        return ((sizeBytes + (bytesPerFrame - 1)) / bytesPerFrame) * bytesPerFrame;
    }

    static List<TrackAttempt> buildTrackAttempts(int channelCount, int sampleRate, int nativeOutputSampleRate,
                                                 int bytesPerFrame, int minBufferSize, boolean enableAudioFx) {
        List<TrackAttempt> attempts = new ArrayList<>();
        if (minBufferSize <= 0 || bytesPerFrame <= 0) {
            return attempts;
        }

        int safeBufferSize = roundUpToFrame(Math.max(minBufferSize, bytesPerFrame * 2), bytesPerFrame);

        if (channelCount > 2) {
            // AudioFlinger's fast mixer only supports mono/stereo, so requesting low-latency
            // multichannel with an undersized 10 ms buffer risks fast-path rejection/downgrade
            // and immediate underruns. Always use at least minBufferSize in standard mode.
            attempts.add(new TrackAttempt(false, safeBufferSize));
            return attempts;
        }

        boolean allowLowLatency = (nativeOutputSampleRate == sampleRate) && !enableAudioFx;
        int smallBufferSize = bytesPerFrame * 2;

        if (allowLowLatency) {
            if (smallBufferSize > 0 && smallBufferSize < safeBufferSize) {
                attempts.add(new TrackAttempt(true, smallBufferSize));
            }
            attempts.add(new TrackAttempt(true, safeBufferSize));
        }

        if (smallBufferSize >= minBufferSize && smallBufferSize < safeBufferSize) {
            attempts.add(new TrackAttempt(false, smallBufferSize));
        }
        attempts.add(new TrackAttempt(false, safeBufferSize));
        return attempts;
    }

    static long computeRecoveryBackoffMs(int failedAttemptCount) {
        if (failedAttemptCount <= 0) {
            return 0;
        }
        int shift = Math.min(failedAttemptCount - 1, 10);
        return Math.min(INITIAL_RECOVERY_BACKOFF_MS << shift, MAX_RECOVERY_BACKOFF_MS);
    }

    private static short clampToShort(int sample) {
        if (sample > Short.MAX_VALUE) {
            return Short.MAX_VALUE;
        }
        if (sample < Short.MIN_VALUE) {
            return Short.MIN_VALUE;
        }
        return (short) sample;
    }

    /**
     * Downmixes interleaved multichannel PCM (Limelight channel order:
     * FL, FR, C, LFE, BL, BR, SL, SR) into stereo PCM.
     */
    static short[] downmixToStereo(short[] input, int inputSampleCount, int inputChannelCount, short[] scratch) {
        int usableSamples = Math.min(inputSampleCount, input.length);
        int frameCount = usableSamples / inputChannelCount;
        int needed = frameCount * 2;
        short[] output = (scratch != null && scratch.length >= needed) ? scratch : new short[needed];

        for (int f = 0; f < frameCount; f++) {
            int inIdx = f * inputChannelCount;
            int outIdx = f * 2;
            int fl = input[inIdx];
            int fr = input[inIdx + 1];
            int left = fl;
            int right = fr;

            if (inputChannelCount == 4) {
                // FL, FR, BL, BR
                int bl = input[inIdx + 2];
                int br = input[inIdx + 3];
                left += (bl * Q15_INV_SQRT2) >> 15;
                right += (br * Q15_INV_SQRT2) >> 15;
            } else if (inputChannelCount >= 6) {
                // FL, FR, C, LFE, BL, BR [, SL, SR]
                int c = input[inIdx + 2];
                int lfe = input[inIdx + 3];
                int bl = input[inIdx + 4];
                int br = input[inIdx + 5];
                int cAtten = (c * Q15_INV_SQRT2) >> 15;
                int lfeAtten = lfe >> 1;
                left += cAtten + lfeAtten + ((bl * Q15_INV_SQRT2) >> 15);
                right += cAtten + lfeAtten + ((br * Q15_INV_SQRT2) >> 15);

                if (inputChannelCount >= 8) {
                    int sl = input[inIdx + 6];
                    int sr = input[inIdx + 7];
                    left += (sl * Q15_INV_SQRT2) >> 15;
                    right += (sr * Q15_INV_SQRT2) >> 15;
                }
            }

            output[outIdx] = clampToShort(left);
            output[outIdx + 1] = clampToShort(right);
        }

        return output;
    }

    private AudioTrack createAudioTrack(int channelConfig, int sampleRate, int bufferSize, boolean lowLatency) {
        AudioAttributes.Builder attributesBuilder = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build();

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            // Use FLAG_LOW_LATENCY on L through N
            if (lowLatency) {
                attributesBuilder.setFlags(AudioAttributes.FLAG_LOW_LATENCY);
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioTrack.Builder trackBuilder = new AudioTrack.Builder()
                    .setAudioFormat(format)
                    .setAudioAttributes(attributesBuilder.build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufferSize);

            // Use PERFORMANCE_MODE_LOW_LATENCY on O and later
            if (lowLatency) {
                trackBuilder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
            }

            return trackBuilder.build();
        }
        else {
            return new AudioTrack(attributesBuilder.build(),
                    format,
                    bufferSize,
                    AudioTrack.MODE_STREAM,
                    AudioManager.AUDIO_SESSION_ID_GENERATE);
        }
    }

    private void openAudioFxSessionLocked() {
        if (enableAudioFx && context != null && !audioFxSessionOpen && audioSessionId != 0) {
            Intent i = new Intent(AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, audioSessionId);
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            i.putExtra(AudioEffect.EXTRA_CONTENT_TYPE, AudioEffect.CONTENT_TYPE_GAME);
            context.sendBroadcast(i);
            audioFxSessionOpen = true;
        }
    }

    private void closeAudioFxSessionLocked() {
        if (enableAudioFx && context != null && audioFxSessionOpen && audioSessionId != 0) {
            Intent i = new Intent(AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, audioSessionId);
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            context.sendBroadcast(i);
            audioFxSessionOpen = false;
        }
    }

    private void releaseTrackLocked() {
        closeAudioFxSessionLocked();
        if (track != null) {
            track.detachRoutingCallback();
            try {
                track.pause();
                track.flush();
            } catch (IllegalStateException ignored) {}
            try {
                track.release();
            } catch (Exception ignored) {}
            track = null;
        }
        audioSessionId = 0;
    }

    private boolean tryOpenTrackForChannelConfig(int channelCount, int channelMask) {
        int bytesPerFrame = channelCount * samplesPerFrame * 2;
        int minBufferSize = backend.getMinBufferSize(sampleRate, channelMask);
        if (minBufferSize <= 0) {
            LimeLog.warning("AudioTrack.getMinBufferSize failed for " + channelCount + "ch: " + minBufferSize);
            return false;
        }

        int nativeSampleRate = backend.getNativeOutputSampleRate();
        List<TrackAttempt> attempts = buildTrackAttempts(
                channelCount, sampleRate, nativeSampleRate, bytesPerFrame, minBufferSize, enableAudioFx);

        for (TrackAttempt attempt : attempts) {
            AudioTrackHandle candidate = null;
            try {
                candidate = backend.createAndStartTrack(channelMask, sampleRate, attempt.bufferSize, attempt.lowLatency);
                final AudioTrackHandle handleRef = candidate;
                synchronized (trackLock) {
                    if (stopped) {
                        try {
                            candidate.pause();
                            candidate.flush();
                        } catch (IllegalStateException ignored) {}
                        try {
                            candidate.release();
                        } catch (Exception ignored) {}
                        return false;
                    }

                    this.track = candidate;
                    this.outputChannelCount = channelCount;
                    this.audioSessionId = candidate.getAudioSessionId();
                    candidate.attachRoutingCallback(new RoutingCallback() {
                        @Override
                        public void onDeviceChanged(int deviceId, int deviceType) {
                            synchronized (trackLock) {
                                if (stopped || track != handleRef || deviceId == 0) {
                                    return;
                                }
                                if (lastRoutedDeviceId == 0) {
                                    lastRoutedDeviceId = deviceId;
                                } else if (lastRoutedDeviceId != deviceId) {
                                    LimeLog.info("Audio routing changed: device " + lastRoutedDeviceId +
                                            " -> " + deviceId + " (type " + deviceType + ")");
                                    lastRoutedDeviceId = deviceId;
                                    routingChanged = true;
                                    recoveryAttempts = 0;
                                    nextRecoveryTimeMs = 0;
                                }
                            }
                        }
                    });
                    if (started) {
                        openAudioFxSessionLocked();
                    }
                }

                LimeLog.info("Audio track configuration: " + channelCount + "ch, buffer=" +
                        attempt.bufferSize + ", lowLatency=" + attempt.lowLatency);
                return true;
            } catch (Exception e) {
                e.printStackTrace();
                if (candidate != null) {
                    try {
                        candidate.release();
                    } catch (Exception ignored) {}
                }
            }
        }

        return false;
    }

    private boolean openAudioTrack() {
        int requestedChannels = requestedAudioConfig.channelCount;
        int requestedMask = channelMaskForCount(requestedChannels);
        if (requestedMask == 0) {
            LimeLog.severe("Decoder returned unhandled channel count: " + requestedChannels);
            return false;
        }

        if (tryOpenTrackForChannelConfig(requestedChannels, requestedMask)) {
            return true;
        }

        if (requestedChannels > 2) {
            LimeLog.warning("Multichannel AudioTrack creation failed for " + requestedChannels +
                    " channels; falling back to stereo downmix");
            return tryOpenTrackForChannelConfig(2, AudioFormat.CHANNEL_OUT_STEREO);
        }

        return false;
    }

    private boolean recreateAudioTrack() {
        synchronized (trackLock) {
            if (stopped) {
                return false;
            }
            releaseTrackLocked();
        }

        if (openAudioTrack()) {
            recoveryAttempts = 0;
            nextRecoveryTimeMs = 0;
            return true;
        }

        recoveryAttempts++;
        nextRecoveryTimeMs = backend.elapsedRealtimeMs() + computeRecoveryBackoffMs(recoveryAttempts);
        return false;
    }

    int getOutputChannelCountForTest() {
        synchronized (trackLock) {
            return outputChannelCount;
        }
    }

    boolean hasActiveTrackForTest() {
        synchronized (trackLock) {
            return track != null;
        }
    }

    @Override
    public int setup(MoonBridge.AudioConfiguration audioConfiguration, int sampleRate, int samplesPerFrame) {
        if (channelMaskForCount(audioConfiguration.channelCount) == 0) {
            LimeLog.severe("Decoder returned unhandled channel count");
            return -1;
        }

        LimeLog.info("Audio channel config: " + String.format("0x%X", channelMaskForCount(audioConfiguration.channelCount)));

        synchronized (trackLock) {
            this.requestedAudioConfig = audioConfiguration;
            this.sampleRate = sampleRate;
            this.samplesPerFrame = samplesPerFrame;
            this.inputChannelCount = audioConfiguration.channelCount;
            this.outputChannelCount = 0;
            this.stopped = false;
            this.started = false;
            this.routingChanged = false;
            this.lastRoutedDeviceId = 0;
            this.recoveryAttempts = 0;
            this.nextRecoveryTimeMs = 0;
            this.audioFxSessionOpen = false;
            this.audioSessionId = 0;
        }

        if (!openAudioTrack()) {
            // Couldn't create any audio track for playback
            return -2;
        }

        return 0;
    }

    @Override
    public void playDecodedAudio(short[] audioData, int sampleCount) {
        if (stopped || audioData == null || sampleCount <= 0) {
            return;
        }

        // Only queue up to 40 ms of pending audio data in addition to what AudioTrack is buffering for us.
        int pendingDuration = backend.getPendingAudioDurationMs();
        if (pendingDuration >= 40) {
            LimeLog.info("Too much pending audio data: " + pendingDuration + " ms");
            return;
        }

        if (routingChanged) {
            routingChanged = false;
            recoveryAttempts = 0;
            nextRecoveryTimeMs = 0;
            if (!recreateAudioTrack()) {
                return;
            }
        } else if (track == null) {
            if (recoveryAttempts >= MAX_RECOVERY_ATTEMPTS ||
                    backend.elapsedRealtimeMs() < nextRecoveryTimeMs) {
                return;
            }
            if (!recreateAudioTrack()) {
                return;
            }
        }

        AudioTrackHandle currentTrack;
        int currentOutputChannels;
        synchronized (trackLock) {
            if (stopped || track == null) {
                return;
            }
            currentTrack = track;
            currentOutputChannels = outputChannelCount;
        }

        int usableSamples = Math.min(sampleCount, audioData.length);
        short[] outputBuffer;
        int outputSampleCount;
        if (currentOutputChannels == 2 && inputChannelCount > 2) {
            downmixBuffer = downmixToStereo(audioData, usableSamples, inputChannelCount, downmixBuffer);
            outputBuffer = downmixBuffer;
            outputSampleCount = (usableSamples / inputChannelCount) * 2;
        } else {
            outputBuffer = audioData;
            outputSampleCount = usableSamples;
        }

        if (outputSampleCount <= 0) {
            return;
        }

        // This will block until the write is completed. That can cause a backlog
        // of pending audio data, so we do the above check to be able to bound
        // latency at 40 ms in that situation.
        int written = currentTrack.write(outputBuffer, 0, outputSampleCount);
        if (stopped) {
            return;
        }

        if (written < 0) {
            LimeLog.warning("AudioTrack.write() failed with error " + written + "; recreating track");
            synchronized (trackLock) {
                if (track == currentTrack) {
                    releaseTrackLocked();
                }
            }

            if (recoveryAttempts < MAX_RECOVERY_ATTEMPTS && recreateAudioTrack() && !stopped) {
                AudioTrackHandle recoveredTrack;
                int recoveredOutputChannels;
                synchronized (trackLock) {
                    recoveredTrack = track;
                    recoveredOutputChannels = outputChannelCount;
                }
                if (recoveredTrack != null) {
                    if (recoveredOutputChannels == 2 && inputChannelCount > 2) {
                        downmixBuffer = downmixToStereo(audioData, usableSamples, inputChannelCount, downmixBuffer);
                        outputBuffer = downmixBuffer;
                        outputSampleCount = (usableSamples / inputChannelCount) * 2;
                    } else {
                        outputBuffer = audioData;
                        outputSampleCount = usableSamples;
                    }
                    if (outputSampleCount > 0) {
                        int retryWritten = recoveredTrack.write(outputBuffer, 0, outputSampleCount);
                        if (retryWritten < 0 && !stopped) {
                            LimeLog.warning("AudioTrack.write() failed after recovery: " + retryWritten);
                            synchronized (trackLock) {
                                if (track == recoveredTrack) {
                                    releaseTrackLocked();
                                }
                            }
                            recoveryAttempts++;
                            nextRecoveryTimeMs = backend.elapsedRealtimeMs() + computeRecoveryBackoffMs(recoveryAttempts);
                        }
                    }
                }
            }
        } else {
            recoveryAttempts = 0;
            nextRecoveryTimeMs = 0;
        }
    }

    @Override
    public void start() {
        synchronized (trackLock) {
            started = true;
            if (!stopped && track != null) {
                openAudioFxSessionLocked();
            }
        }
    }

    @Override
    public void stop() {
        synchronized (trackLock) {
            stopped = true;
            started = false;
            closeAudioFxSessionLocked();
            if (track != null) {
                // Immediately unblock any pending blocking AudioTrack.write() on the decoder thread.
                try {
                    track.pause();
                    track.flush();
                } catch (IllegalStateException ignored) {}
            }
        }
    }

    @Override
    public void cleanup() {
        synchronized (trackLock) {
            stopped = true;
            started = false;
            releaseTrackLocked();
        }
    }
}
