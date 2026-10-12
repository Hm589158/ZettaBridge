package com.zettabridge.launcher;

import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import java.lang.reflect.Field;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.locks.LockSupport;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Holds a translated guest's GLSurfaceView to at most 60 frames per second.
 *
 * The 32-bit games we translate were written for 60 Hz screens, and many advance their world by
 * one fixed step per rendered frame. GLSurfaceView renders once per vsync, so on a 120 Hz panel
 * such a game runs at double speed. Asking for a 60 Hz display mode (GuestWindowStyle) is not
 * enough: HyperOS, for one, keeps the panel at 120 Hz regardless. So the renderer the game
 * installed is wrapped, and each onDrawFrame waits until a 60 Hz frame slot is due. A game that
 * already gets 60 Hz frames, or is slower than that, never waits.
 */
final class FramePacer {
    private static final String TAG = "zb-launcher";
    private static final long FRAME_NANOS = 1_000_000_000L / 60;

    private static final Set<Activity> watched = Collections.newSetFromMap(new WeakHashMap<>());
    private static Field rendererField;
    private static boolean rendererFieldMissing;

    private FramePacer() {}

    /** Paces every GLSurfaceView the activity shows, now and whenever its views change. */
    static void watch(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        View decor = window.getDecorView();
        scan(decor);
        if (watched.add(activity)) {
            // Games often build their GLSurfaceView after onCreate (after loading, in onResume).
            decor.getViewTreeObserver().addOnGlobalLayoutListener(() -> scan(decor));
        }
    }

    private static void scan(View view) {
        if (view instanceof GLSurfaceView) {
            wrap((GLSurfaceView) view);
        } else if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) scan(group.getChildAt(i));
        }
    }

    private static void wrap(GLSurfaceView view) {
        Field field = rendererField();
        if (field == null) return;
        try {
            Object renderer = field.get(view);
            if (renderer == null || renderer instanceof PacedRenderer) return;
            // GLThread reads mRenderer for every frame, so the swap applies to a running thread too.
            field.set(view, new PacedRenderer((GLSurfaceView.Renderer) renderer));
            Log.i(TAG, "frame pacing: " + renderer.getClass().getName() + " held to 60 fps");
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "cannot pace " + view.getClass().getName() + ": " + e);
        }
    }

    private static synchronized Field rendererField() {
        if (rendererField == null && !rendererFieldMissing) {
            try {
                rendererField = Reflect.field(GLSurfaceView.class, "mRenderer");
            } catch (NoSuchFieldException e) {
                rendererFieldMissing = true;
                Log.w(TAG, "GLSurfaceView.mRenderer not found; frame pacing is off");
            }
        }
        return rendererField;
    }

    private static final class PacedRenderer implements GLSurfaceView.Renderer {
        private final GLSurfaceView.Renderer inner;
        private long next;  // System.nanoTime() of the next 60 Hz frame slot; touched only on GLThread

        PacedRenderer(GLSurfaceView.Renderer inner) {
            this.inner = inner;
        }

        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            next = 0;
            inner.onSurfaceCreated(gl, config);
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            inner.onSurfaceChanged(gl, width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            long now = System.nanoTime();
            if (next != 0 && now < next) {
                LockSupport.parkNanos(next - now);
                now = System.nanoTime();
            }
            // Keep the cadence, but after a stall (a pause, a slow frame) start again from now
            // instead of rushing frames to catch up.
            next = next == 0 || now - next > FRAME_NANOS ? now + FRAME_NANOS : next + FRAME_NANOS;
            inner.onDrawFrame(gl);
        }
    }
}
