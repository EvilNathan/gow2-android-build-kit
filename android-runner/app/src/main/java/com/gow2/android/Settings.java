package com.gow2.android;

import org.json.JSONException;
import org.json.JSONObject;

/**
 * User-visible port settings.  The same field set is serialised to JSON and handed to the native host
 * (see nativeStart), which maps it onto engine environment variables.
 *
 * <p>Defaults mirror the values the native host expects when no settings file exists.
 */
public final class Settings {

    public static final String ASPECT_FILL = "fill";
    public static final String ASPECT_4X3 = "4:3";
    public static final String ASPECT_FIT = "fit";

    public static final String PRESET_PERFORMANCE = "performance";
    public static final String PRESET_BALANCED = "balanced";
    public static final String PRESET_QUALITY = "quality";

    public static final String LOG_INFO = "info";
    public static final String LOG_DEBUG = "debug";
    public static final String LOG_WARN = "warn";
    public static final String LOG_ERROR = "error";

    /** Internal GS resolution multiplier (PS2X_GS_HW_SCALE). */
    public float scale = 1.0f;
    public String aspect = ASPECT_FILL;
    public boolean vsync = true;
    public String preset = PRESET_BALANCED;
    public float volume = 0.8f;
    public String padScript = "";
    /** Informational: the SAF URI the user picked, kept so re-imports are possible. */
    public String isoUri = "";
    public String logLevel = LOG_INFO;
    public float touchScale = 1.0f;
    public float touchOpacity = 0.6f;
    public boolean leftHanded = false;
    public float deadzone = 0.18f;

    public Settings() {
    }

    public Settings copy() {
        Settings other = new Settings();
        other.scale = scale;
        other.aspect = aspect;
        other.vsync = vsync;
        other.preset = preset;
        other.volume = volume;
        other.padScript = padScript;
        other.isoUri = isoUri;
        other.logLevel = logLevel;
        other.touchScale = touchScale;
        other.touchOpacity = touchOpacity;
        other.leftHanded = leftHanded;
        other.deadzone = deadzone;
        return other;
    }

    /** Hardware scale implied by a graphics preset. */
    public static float presetScale(String preset) {
        if (PRESET_PERFORMANCE.equals(preset)) {
            return 0.75f;
        }
        if (PRESET_QUALITY.equals(preset)) {
            return 1.5f;
        }
        return 1.0f;
    }

    public void applyPreset(String preset) {
        this.preset = sanitizePreset(preset);
        this.scale = presetScale(this.preset);
    }

    /** Clamps every field into a range the host and the UI can both handle. */
    public void sanitize() {
        scale = clamp(scale <= 0.0f ? presetScale(preset) : scale, 0.5f, 2.0f);
        volume = clamp(volume, 0.0f, 1.0f);
        touchScale = clamp(touchScale, 0.7f, 1.6f);
        touchOpacity = clamp(touchOpacity, 0.15f, 1.0f);
        deadzone = clamp(deadzone, 0.0f, 0.45f);
        preset = sanitizePreset(preset);
        aspect = sanitizeAspect(aspect);
        logLevel = sanitizeLogLevel(logLevel);
        if (padScript == null) {
            padScript = "";
        }
        if (isoUri == null) {
            isoUri = "";
        }
    }

    public String toJson() {
        StringBuilder json = new StringBuilder(320);
        json.append('{');
        appendFloat(json, "scale", scale).append(',');
        appendString(json, "aspect", aspect).append(',');
        json.append("\"vsync\":").append(vsync).append(',');
        appendString(json, "preset", preset).append(',');
        appendFloat(json, "volume", volume).append(',');
        appendString(json, "padScript", padScript).append(',');
        appendString(json, "isoUri", isoUri).append(',');
        appendString(json, "logLevel", logLevel).append(',');
        appendFloat(json, "touchScale", touchScale).append(',');
        appendFloat(json, "touchOpacity", touchOpacity).append(',');
        json.append("\"leftHanded\":").append(leftHanded).append(',');
        appendFloat(json, "deadzone", deadzone);
        json.append('}');
        return json.toString();
    }

    public static Settings fromJson(String json) {
        Settings settings = new Settings();
        if (json == null || json.isEmpty()) {
            return settings;
        }
        try {
            JSONObject object = new JSONObject(json);
            settings.scale = (float) object.optDouble("scale", settings.scale);
            settings.aspect = object.optString("aspect", settings.aspect);
            settings.vsync = object.optBoolean("vsync", settings.vsync);
            settings.preset = object.optString("preset", settings.preset);
            settings.volume = (float) object.optDouble("volume", settings.volume);
            settings.padScript = object.optString("padScript", settings.padScript);
            settings.isoUri = object.optString("isoUri", settings.isoUri);
            settings.logLevel = object.optString("logLevel", settings.logLevel);
            settings.touchScale = (float) object.optDouble("touchScale", settings.touchScale);
            settings.touchOpacity = (float) object.optDouble("touchOpacity", settings.touchOpacity);
            settings.leftHanded = object.optBoolean("leftHanded", settings.leftHanded);
            settings.deadzone = (float) object.optDouble("deadzone", settings.deadzone);
        } catch (JSONException ignored) {
            // Corrupt file: fall back to defaults rather than refusing to launch.
        }
        settings.sanitize();
        return settings;
    }

    private static StringBuilder appendFloat(StringBuilder json, String key, float value) {
        appendQuoted(json, key).append(':').append(Float.toString(value));
        return json;
    }

    private static StringBuilder appendString(StringBuilder json, String key, String value) {
        appendQuoted(json, key).append(':');
        return appendQuoted(json, value == null ? "" : value);
    }

    private static StringBuilder appendQuoted(StringBuilder json, String value) {
        json.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"':
                    json.append("\\\"");
                    break;
                case '\\':
                    json.append("\\\\");
                    break;
                case '\n':
                    json.append("\\n");
                    break;
                case '\r':
                    json.append("\\r");
                    break;
                case '\t':
                    json.append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        json.append(String.format(java.util.Locale.US, "\\u%04x", (int) c));
                    } else {
                        json.append(c);
                    }
                    break;
            }
        }
        json.append('"');
        return json;
    }

    private static String sanitizePreset(String value) {
        if (PRESET_PERFORMANCE.equals(value) || PRESET_QUALITY.equals(value)) {
            return value;
        }
        return PRESET_BALANCED;
    }

    private static String sanitizeAspect(String value) {
        if (ASPECT_4X3.equals(value) || ASPECT_FIT.equals(value)) {
            return value;
        }
        return ASPECT_FILL;
    }

    private static String sanitizeLogLevel(String value) {
        if (LOG_DEBUG.equals(value) || LOG_WARN.equals(value) || LOG_ERROR.equals(value)) {
            return value;
        }
        return LOG_INFO;
    }

    static float clamp(float value, float min, float max) {
        if (Float.isNaN(value)) {
            return min;
        }
        return Math.max(min, Math.min(max, value));
    }
}
