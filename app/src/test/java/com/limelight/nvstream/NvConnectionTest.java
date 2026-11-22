package com.limelight.nvstream;

import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.av.video.VideoDecoderRenderer;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.jni.MoonBridge;

import org.junit.Test;

import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.Assert.*;

public class NvConnectionTest {

    private static final class TestListener implements NvConnectionListener {
        final CountDownLatch failedLatch = new CountDownLatch(1);
        @Override public void stageStarting(String stage) {}
        @Override public void stageComplete(String stage) {}
        @Override public void stageFailed(String stage, int portFlags, int errorCode) { failedLatch.countDown(); }
        @Override public void connectionStarted() {}
        @Override public void connectionTerminated(int errorCode) {}
        @Override public void displayMessage(String message) {}
        @Override public void displayTransientMessage(String message) {}
        @Override public void rumble(short controllerNumber, short lowFreqMotor, short highFreqMotor) {}
        @Override public void rumbleTriggers(short controllerNumber, short leftTrigger, short rightTrigger) {}
        @Override public void setHdrMode(boolean enabled, byte[] hdrMetadata) {}
        @Override public void setMotionEventState(short controllerNumber, byte motionType, short reportRateHz) {}
        @Override public void setControllerLED(short controllerNumber, byte r, byte g, byte b) {}
        @Override public void connectionStatusUpdate(int connectionStatus) {}
    }

    private static StreamConfiguration testStreamConfig(MoonBridge.AudioConfiguration audioConfig) {
        return new StreamConfiguration.Builder()
                .setApp(new NvApp("Test", 1, false))
                .setAudioConfiguration(audioConfig)
                .build();
    }

    @Test
    public void nvConnectionAcquiresPermitBeforeStartAppAndCleansUpOnFailure() throws Exception {
        final CountDownLatch conn1Started = new CountDownLatch(1);
        final CountDownLatch allowConn1StopFinish = new CountDownLatch(1);
        final AtomicBoolean conn1Stopped = new AtomicBoolean(false);
        final AtomicBoolean conn2SawConn1Stopped = new AtomicBoolean(false);
        final AtomicInteger cleanupCalls = new AtomicInteger(0);

        NvConnection.Bridge bridge1 = new NvConnection.Bridge() {
            @Override public void interruptConnection() {}
            @Override public void setupBridge(VideoDecoderRenderer v, AudioRenderer a, NvConnectionListener l) {}
            @Override public int startConnection(String a, String b, String c, String d, int e, int f, int g, int h, int i, int j, int k, int l, int m, int n, byte[] o, byte[] p, int q, int r, int s) {
                conn1Started.countDown();
                return 0;
            }
            @Override public void stopConnection() {
                try {
                    allowConn1StopFinish.await(2, TimeUnit.SECONDS);
                } catch (InterruptedException ignored) {}
                conn1Stopped.set(true);
            }
            @Override public void cleanupBridge() {
                cleanupCalls.incrementAndGet();
            }
        };

        NvConnection conn1 = new NvConnection(null, new ComputerDetails.AddressTuple("127.0.0.1", 47989),
                47984, "uid", testStreamConfig(MoonBridge.AUDIO_CONFIGURATION_51_SURROUND), null, null, bridge1) {
            @Override
            protected boolean startApp() {
                return true;
            }
        };

        TestListener listener1 = new TestListener();
        conn1.start(null, null, listener1);
        assertTrue(conn1Started.await(2, TimeUnit.SECONDS));

        Thread stopThread = new Thread(conn1::stop);
        stopThread.start();

        NvConnection.Bridge bridge2 = new NvConnection.Bridge() {
            @Override public void interruptConnection() {}
            @Override public void setupBridge(VideoDecoderRenderer v, AudioRenderer a, NvConnectionListener l) {}
            @Override public int startConnection(String a, String b, String c, String d, int e, int f, int g, int h, int i, int j, int k, int l, int m, int n, byte[] o, byte[] p, int q, int r, int s) {
                return -1;
            }
            @Override public void stopConnection() {}
            @Override public void cleanupBridge() {
                cleanupCalls.incrementAndGet();
            }
        };

        final CountDownLatch conn2Done = new CountDownLatch(1);
        NvConnection conn2 = new NvConnection(null, new ComputerDetails.AddressTuple("127.0.0.1", 47989),
                47984, "uid", testStreamConfig(MoonBridge.AUDIO_CONFIGURATION_STEREO), null, null, bridge2) {
            @Override
            protected boolean startApp() {
                conn2SawConn1Stopped.set(conn1Stopped.get());
                conn2Done.countDown();
                return true;
            }
        };

        TestListener listener2 = new TestListener();
        conn2.start(null, null, listener2);

        Thread.sleep(50);
        assertFalse(conn2SawConn1Stopped.get());
        allowConn1StopFinish.countDown();

        assertTrue(conn2Done.await(2, TimeUnit.SECONDS));
        stopThread.join(2000);
        assertTrue(conn2SawConn1Stopped.get());

        Thread.sleep(50);
        assertEquals(2, cleanupCalls.get());
        assertEquals(1, NvConnection.availableConnectionPermitsForTest());

        // Calling conn1.stop() a second time must not double-release permit
        conn1.stop();
        assertEquals(1, NvConnection.availableConnectionPermitsForTest());

        NvConnection conn3 = new NvConnection(null, new ComputerDetails.AddressTuple("127.0.0.1", 47989),
                47984, "uid", testStreamConfig(MoonBridge.AUDIO_CONFIGURATION_STEREO), null, null, bridge2) {
            @Override
            protected boolean startApp() throws IOException {
                throw new IOException("boom");
            }
        };
        TestListener listener3 = new TestListener();
        conn3.start(null, null, listener3);
        assertTrue(listener3.failedLatch.await(2, TimeUnit.SECONDS));
        Thread.sleep(50);
        assertEquals(1, NvConnection.availableConnectionPermitsForTest());

        final CountDownLatch conn4Ran = new CountDownLatch(1);
        NvConnection conn4 = new NvConnection(null, new ComputerDetails.AddressTuple("127.0.0.1", 47989),
                47984, "uid", testStreamConfig(MoonBridge.AUDIO_CONFIGURATION_STEREO), null, null, bridge2) {
            @Override
            protected boolean startApp() {
                conn4Ran.countDown();
                return false;
            }
        };
        TestListener listener4 = new TestListener();
        conn4.start(null, null, listener4);
        assertTrue(conn4Ran.await(2, TimeUnit.SECONDS));
        assertTrue(listener4.failedLatch.await(2, TimeUnit.SECONDS));
        Thread.sleep(50);
        assertEquals(1, NvConnection.availableConnectionPermitsForTest());
    }
}
