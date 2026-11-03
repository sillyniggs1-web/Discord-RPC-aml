package com.hexed.rpc;

import android.app.Activity;
import android.content.Context;

import java.lang.reflect.Field;
import java.util.Map;

/**
 * Loaded by the stub through a DexClassLoader together with the Discord Social SDK classes.
 * Does the two things the SDK needs from Java inside another app's process:
 *  - hands it the game's Activity (DiscordSocialSdkInit.setEngineActivity)
 *  - System.load()s the core library from THIS class loader, so the SDK and the core share one
 *    copy of libdiscord_partner_sdk.so and the same JNI class cache.
 */
public final class HxBridge {
    private HxBridge() {}

    /** Implemented in libhxrpc_core.so; returns the address of its HxApi function table. */
    public static native long apiTable();

    public static String init(Context ctx, String corePath) {
        try {
            Activity act = (ctx instanceof Activity) ? (Activity) ctx : findActivity();
            if (act == null) return "no running Activity found";
            // triggers System.loadLibrary("discord_partner_sdk") from this class loader
            com.discord.socialsdk.DiscordSocialSdkInit.setEngineActivity(act);
            System.load(corePath);
            return null;
        } catch (Throwable t) {
            return t.getClass().getSimpleName() + ": " + t.getMessage();
        }
    }

    private static Activity findActivity() throws Exception {
        Class<?> at = Class.forName("android.app.ActivityThread");
        Object thread = at.getMethod("currentActivityThread").invoke(null);
        Field f = at.getDeclaredField("mActivities");
        f.setAccessible(true);
        Map<?, ?> map = (Map<?, ?>) f.get(thread);
        if (map == null) return null;
        for (Object rec : map.values()) {
            Field af = rec.getClass().getDeclaredField("activity");
            af.setAccessible(true);
            Object a = af.get(rec);
            if (a instanceof Activity) return (Activity) a;
        }
        return null;
    }
}
