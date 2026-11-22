package com.limelight;

import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.preferences.PreferenceConfiguration;

import org.junit.Test;

import static org.junit.Assert.*;

public class GameConfigChangeTest {
    @Test
    public void gamePrefChangeDetectionHandlesAudioAndIgnoresFramePacingMutation() {
        PreferenceConfiguration a = new PreferenceConfiguration();
        a.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_51_SURROUND;
        a.width = 1920;
        a.height = 1080;
        a.fps = 60;
        a.bitrate = 20000;
        a.framePacing = PreferenceConfiguration.FRAME_PACING_BALANCED;

        PreferenceConfiguration b = new PreferenceConfiguration();
        b.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_51_SURROUND;
        b.width = 1920;
        b.height = 1080;
        b.fps = 60;
        b.bitrate = 20000;
        b.framePacing = PreferenceConfiguration.FRAME_PACING_CAP_FPS;

        assertFalse(Game.hasAudioConfigChanged(a, b));
        assertFalse(Game.hasStreamConfigChanged(a, b));

        b.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_STEREO;
        assertTrue(Game.hasAudioConfigChanged(a, b));
        assertTrue(Game.hasStreamConfigChanged(a, b));

        b.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_51_SURROUND;
        b.enableAudioFx = true;
        assertTrue(Game.hasAudioConfigChanged(a, b));
    }
}
