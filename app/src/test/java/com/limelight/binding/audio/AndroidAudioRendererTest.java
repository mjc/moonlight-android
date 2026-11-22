package com.limelight.binding.audio;

import android.media.AudioFormat;
import android.media.AudioTrack;

import com.limelight.nvstream.jni.MoonBridge;

import org.junit.Test;

import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.*;

public class AndroidAudioRendererTest {

    private static final class FakeTrackHandle implements AndroidAudioRenderer.AudioTrackHandle {
        final int channelMask;
        final int bufferSize;
        final boolean lowLatency;
        final List<Integer> writtenSampleCounts = new ArrayList<>();
        final List<short[]> writtenSamples = new ArrayList<>();
        int nextWriteReturn = Integer.MIN_VALUE;
        boolean paused = false;
        boolean flushed = false;
        boolean released = false;
        AndroidAudioRenderer.RoutingCallback routingCallback;

        FakeTrackHandle(int channelMask, int bufferSize, boolean lowLatency) {
            this.channelMask = channelMask;
            this.bufferSize = bufferSize;
            this.lowLatency = lowLatency;
        }

        @Override
        public int write(short[] audioData, int offsetInShorts, int sizeInShorts) {
            if (nextWriteReturn != Integer.MIN_VALUE) {
                int ret = nextWriteReturn;
                nextWriteReturn = Integer.MIN_VALUE;
                return ret;
            }
            writtenSampleCounts.add(sizeInShorts);
            short[] copy = new short[sizeInShorts];
            System.arraycopy(audioData, offsetInShorts, copy, 0, sizeInShorts);
            writtenSamples.add(copy);
            return sizeInShorts;
        }

        @Override
        public int getAudioSessionId() {
            return 101;
        }

        @Override
        public void pause() {
            paused = true;
        }

        @Override
        public void flush() {
            flushed = true;
        }

        @Override
        public void release() {
            released = true;
        }

        @Override
        public void attachRoutingCallback(AndroidAudioRenderer.RoutingCallback callback) {
            this.routingCallback = callback;
        }

        @Override
        public void detachRoutingCallback() {
            this.routingCallback = null;
        }
    }

    private static final class FakeAudioBackend implements AndroidAudioRenderer.AudioTrackBackend {
        int stereoMinBufferSize = 3840;
        int surroundMinBufferSize = 11520;
        int nativeSampleRate = 48000;
        long nowMs = 1000;
        int pendingDurationMs = 0;
        boolean failMultichannel = false;
        boolean failStereo = false;
        final List<FakeTrackHandle> createdTracks = new ArrayList<>();

        @Override
        public int getMinBufferSize(int sampleRate, int channelMask) {
            if (channelMask == AudioFormat.CHANNEL_OUT_STEREO) {
                return stereoMinBufferSize;
            }
            return surroundMinBufferSize;
        }

        @Override
        public int getNativeOutputSampleRate() {
            return nativeSampleRate;
        }

        @Override
        public long elapsedRealtimeMs() {
            return nowMs;
        }

        @Override
        public int getPendingAudioDurationMs() {
            return pendingDurationMs;
        }

        @Override
        public AndroidAudioRenderer.AudioTrackHandle createAndStartTrack(int channelMask, int sampleRate, int bufferSize, boolean lowLatency) {
            if (channelMask != AudioFormat.CHANNEL_OUT_STEREO && failMultichannel) {
                throw new UnsupportedOperationException("Multichannel rejected");
            }
            if (channelMask == AudioFormat.CHANNEL_OUT_STEREO && failStereo) {
                throw new UnsupportedOperationException("Stereo rejected");
            }
            FakeTrackHandle handle = new FakeTrackHandle(channelMask, bufferSize, lowLatency);
            createdTracks.add(handle);
            return handle;
        }
    }

    @Test
    public void buildTrackAttemptsDoesNotMaskBadMinBufferSize() {
        List<AndroidAudioRenderer.TrackAttempt> attempts = AndroidAudioRenderer.buildTrackAttempts(
                6, 48000, 48000, 6 * 240 * 2, AudioTrack.ERROR_BAD_VALUE, false);
        assertTrue(attempts.isEmpty());
    }

    @Test
    public void buildTrackAttemptsAvoidsUndersizedLowLatencyForMultichannel() {
        int bytesPerFrame = 6 * 240 * 2; // 2880 bytes (5ms)
        int minBufferSize = 11520; // 20ms
        List<AndroidAudioRenderer.TrackAttempt> attempts = AndroidAudioRenderer.buildTrackAttempts(
                6, 48000, 48000, bytesPerFrame, minBufferSize, false);
        assertEquals(1, attempts.size());
        assertFalse(attempts.get(0).lowLatency);
        assertTrue(attempts.get(0).bufferSize >= minBufferSize);
        assertEquals(0, attempts.get(0).bufferSize % bytesPerFrame);
    }

    @Test
    public void setupFallsBackToStereoDownmixWhenMultichannelRejected() {
        FakeAudioBackend backend = new FakeAudioBackend();
        backend.surroundMinBufferSize = AudioTrack.ERROR_BAD_VALUE; // -2
        AndroidAudioRenderer renderer = new AndroidAudioRenderer(null, false, backend);

        assertEquals(0, renderer.setup(MoonBridge.AUDIO_CONFIGURATION_51_SURROUND, 48000, 240));
        assertEquals(2, renderer.getOutputChannelCountForTest());
        assertEquals(1, backend.createdTracks.size());
        assertEquals(AudioFormat.CHANNEL_OUT_STEREO, backend.createdTracks.get(0).channelMask);

        short[] pcm = new short[6 * 4];
        pcm[0] = 1000;
        pcm[1] = 2000;
        pcm[2] = 1000;
        pcm[3] = 400;
        pcm[4] = 1000;
        pcm[5] = 1000;
        // Trailing stale samples past sampleCount == 12 must not be written
        pcm[12] = 12345;
        pcm[13] = 12345;

        renderer.playDecodedAudio(pcm, 12);
        FakeTrackHandle track = backend.createdTracks.get(0);
        assertEquals(1, track.writtenSampleCounts.size());
        assertEquals(Integer.valueOf(4), track.writtenSampleCounts.get(0));
        short[] out = track.writtenSamples.get(0);
        assertTrue(out[0] > 2000);
        assertTrue(out[1] > 3000);
        assertEquals(0, out[2]);
        assertEquals(0, out[3]);

        renderer.stop();
        assertTrue(track.paused);
        assertTrue(track.flushed);
        renderer.cleanup();
        assertTrue(track.released);
        assertFalse(renderer.hasActiveTrackForTest());
        renderer.cleanup();
    }

    @Test
    public void deadObjectAndRoutingRecoveryWithBoundedBackoff() {
        FakeAudioBackend backend = new FakeAudioBackend();
        AndroidAudioRenderer renderer = new AndroidAudioRenderer(null, false, backend);

        assertEquals(0, renderer.setup(MoonBridge.AUDIO_CONFIGURATION_51_SURROUND, 48000, 240));
        assertEquals(6, renderer.getOutputChannelCountForTest());
        FakeTrackHandle firstTrack = backend.createdTracks.get(0);
        firstTrack.routingCallback.onDeviceChanged(10, 9);

        firstTrack.nextWriteReturn = AudioTrack.ERROR_DEAD_OBJECT;
        backend.failMultichannel = true;

        short[] frame = new short[6 * 240];
        frame[0] = 500;
        frame[1] = 500;
        renderer.playDecodedAudio(frame, frame.length);

        assertTrue(firstTrack.released);
        assertEquals(2, backend.createdTracks.size());
        assertEquals(2, renderer.getOutputChannelCountForTest());
        FakeTrackHandle stereoTrack = backend.createdTracks.get(1);
        assertEquals(1, stereoTrack.writtenSampleCounts.size());
        assertEquals(Integer.valueOf(2 * 240), stereoTrack.writtenSampleCounts.get(0));

        // Routing change back to HDMI AVR that supports 5.1 upgrades back to 5.1
        backend.failMultichannel = false;
        stereoTrack.routingCallback.onDeviceChanged(20, 9);
        renderer.playDecodedAudio(frame, frame.length);
        assertTrue(stereoTrack.released);
        assertEquals(3, backend.createdTracks.size());
        assertEquals(6, renderer.getOutputChannelCountForTest());

        // Bounded backoff when both multichannel and stereo fail
        FakeTrackHandle thirdTrack = backend.createdTracks.get(2);
        thirdTrack.nextWriteReturn = AudioTrack.ERROR_DEAD_OBJECT;
        backend.failMultichannel = true;
        backend.failStereo = true;
        renderer.playDecodedAudio(frame, frame.length);
        assertFalse(renderer.hasActiveTrackForTest());
        int tracksAfterFirstFailure = backend.createdTracks.size();

        renderer.playDecodedAudio(frame, frame.length);
        assertEquals(tracksAfterFirstFailure, backend.createdTracks.size());

        backend.failStereo = false;
        backend.nowMs += 5000;
        renderer.playDecodedAudio(frame, frame.length);
        assertTrue(renderer.hasActiveTrackForTest());
    }
}
