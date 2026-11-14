package app.rincast.vr;

import android.Manifest;
import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.speech.tts.TextToSpeech;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/**
 * The whole app is native (OpenXR + OpenGL ES in librincast.so). This class only
 * provides the few platform services that have no NDK equivalent: HTTPS,
 * runtime permissions, Wi-Fi performance locks and a stable device id.
 * Native code calls these methods through JNI.
 */
public class MainActivity extends NativeActivity {
    static {
        System.loadLibrary("rincast");
    }

    private WifiManager.WifiLock wifiLock;
    private WifiManager.WifiLock wifiHighPerfLock;
    private WifiManager.MulticastLock multicastLock;
    private PowerManager.WakeLock wakeLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Permissions are asked for by the native side once the first frame is up: PICO's VR
        // permission screen takes over the display, and asking here (before anything is drawn)
        // is what left the app looking like it wouldn't open after an update.
    }

    @Override
    protected void onDestroy() {
        setStreamingLocks(false);
        setUsbNetwork(false);
        super.onDestroy();
    }

    // ---- A USB cable to the PC (Horizon OS 2.5+) ------------------------------------
    // The headset offers the cable as a network of its own: IPv6 link-local, no internet.
    // Asked for once; the user allows it in a system dialog, which only shows while RinCast is
    // in front. Native code polls usbNetworkState().

    private ConnectivityManager.NetworkCallback usbCallback;
    private volatile int usbState = 0;  // 0 off, 1 waiting for a cable, 2 connected, 3 not allowed
    private volatile long usbHandle = 0;
    private volatile String usbInterface = "";
    private volatile int usbKbps = 0;

    public synchronized void setUsbNetwork(boolean on) {
        final ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        if (!on) {
            if (usbCallback != null) {
                try { cm.unregisterNetworkCallback(usbCallback); } catch (RuntimeException ignored) { }
                usbCallback = null;
            }
            usbState = 0;
            usbHandle = 0;
            usbInterface = "";
            return;
        }
        if (usbCallback != null) return;
        final NetworkRequest request = new NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_USB)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
            .build();
        usbCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) {
                usbHandle = network.getNetworkHandle();
                LinkProperties lp = cm.getLinkProperties(network);
                if (lp != null && lp.getInterfaceName() != null) usbInterface = lp.getInterfaceName();
                usbState = 2;
            }
            @Override public void onLinkPropertiesChanged(Network network, LinkProperties lp) {
                if (lp.getInterfaceName() != null) usbInterface = lp.getInterfaceName();
            }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                usbKbps = caps.getLinkDownstreamBandwidthKbps();
            }
            @Override public void onLost(Network network) {
                usbHandle = 0;
                usbState = 1;
            }
            @Override public void onUnavailable() {
                // Declined, or asked while in the background: the request is gone.
                synchronized (MainActivity.this) { usbCallback = null; }
                usbHandle = 0;
                usbState = 3;
            }
        };
        usbState = 1;
        try {
            cm.requestNetwork(request, usbCallback);
        } catch (RuntimeException e) {
            usbCallback = null;
            usbState = 3;
        }
    }

    /** "state,handle,kbps,interface". */
    public String usbNetworkState() {
        return usbState + "," + usbHandle + "," + usbKbps + "," + usbInterface;
    }

    /** Stable random id for this installation (used to recognize the headset). */
    public String getRinCastDeviceId() {
        SharedPreferences prefs = getSharedPreferences("rincast", Context.MODE_PRIVATE);
        String id = prefs.getString("deviceId", null);
        if (id == null) {
            id = UUID.randomUUID().toString().replace("-", "").substring(0, 24);
            prefs.edit().putString("deviceId", id).apply();
        }
        return id;
    }

    public String getDeviceModel() {
        return Build.MODEL;
    }

    public String getFilesPath() {
        return getFilesDir().getAbsolutePath();
    }

    /** The system's language as a BCP 47 tag, e.g. "zh-CN", for the UI's "auto" language. */
    public String getSystemLocale() {
        return android.content.res.Resources.getSystem().getConfiguration().getLocales().get(0).toLanguageTag();
    }

    public boolean hasMicPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Keeps Wi-Fi in its low-latency mode and the CPU awake while streaming;
     * the multicast lock lets us receive LAN discovery broadcasts.
     */
    public synchronized void setStreamingLocks(boolean enable) {
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (enable) {
            if (wifiLock == null && wifi != null) {
                int mode = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                       : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                wifiLock = wifi.createWifiLock(mode, "rincast:stream");
                wifiLock.acquire();
            }
            // The low-latency lock only counts while Android sees the app in the foreground
            // with the screen on, which headsets don't all report while it's in VR. Holding the
            // high-performance one as well keeps the radio out of power save there too.
            if (wifiHighPerfLock == null && wifi != null && Build.VERSION.SDK_INT >= 29) {
                wifiHighPerfLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "rincast:stream-hp");
                wifiHighPerfLock.acquire();
            }
            if (multicastLock == null && wifi != null) {
                multicastLock = wifi.createMulticastLock("rincast:discovery");
                multicastLock.acquire();
            }
            if (wakeLock == null) {
                PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "rincast:stream");
                wakeLock.acquire();
            }
        } else {
            if (wifiLock != null) { wifiLock.release(); wifiLock = null; }
            if (wifiHighPerfLock != null) { wifiHighPerfLock.release(); wifiHighPerfLock = null; }
            if (multicastLock != null) { multicastLock.release(); multicastLock = null; }
            if (wakeLock != null) { wakeLock.release(); wakeLock = null; }
        }
    }

    // ---- Interface sounds and spoken labels -----------------------------------------

    private final AudioTrack[] uiSounds = new AudioTrack[3];
    private TextToSpeech tts;
    private volatile boolean ttsReady = false;
    private volatile String pendingSpeech;

    // A short soft tone: one note per frequency, each with a fast decay.
    private static AudioTrack makeSound(double[] notes, int noteMs, double volume) {
        final int rate = 48000, noteFrames = rate * noteMs / 1000;
        final short[] pcm = new short[noteFrames * notes.length];
        for (int n = 0; n < notes.length; n++) {
            for (int i = 0; i < noteFrames; i++) {
                final double t = (double) i / rate;
                final double envelope = Math.min(1.0, i / 96.0) * Math.exp(-t * 2000.0 / noteMs / 4.0);
                pcm[n * noteFrames + i] = (short) (Math.sin(2 * Math.PI * notes[n] * t) * envelope * volume * 32767);
            }
        }
        final AudioTrack track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(pcm.length * 2)
            .setTransferMode(AudioTrack.MODE_STATIC).build();
        track.write(pcm, 0, pcm.length);
        return track;
    }

    public void playUiSound(int kind) {
        try {
            if (uiSounds[0] == null) {
                uiSounds[0] = makeSound(new double[] {1400}, 25, 0.12);
                uiSounds[1] = makeSound(new double[] {660, 990}, 90, 0.2);
                uiSounds[2] = makeSound(new double[] {440, 330}, 130, 0.22);
            }
            final AudioTrack t = uiSounds[kind];
            t.stop();
            t.reloadStaticData();
            t.play();
        } catch (RuntimeException e) {
            // Sounds are a nicety: never let them disturb the app.
        }
    }

    /** Whether the app can read the shared storage the screenshots feature needs. */
    public boolean screenshotStorageOk() {
        if (android.os.Build.VERSION.SDK_INT >= 30 && android.os.Environment.isExternalStorageManager()) return true;
        final String p = android.os.Build.VERSION.SDK_INT >= 33
                ? "android.permission.READ_MEDIA_IMAGES" : Manifest.permission.READ_EXTERNAL_STORAGE;
        return checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED;
    }

    /** The system's file-access page for this app, where the screenshots permission lives. */
    public void openStorageSettings() {
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     android.net.Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException e) {
            try { startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            catch (RuntimeException ignored) {}
        }
    }

    /** Asks for those of the permissions (separated by commas) that aren't granted yet. */
    public void requestPermissionList(String list) {
        final java.util.ArrayList<String> missing = new java.util.ArrayList<>();
        for (String p : list.split(",")) {
            // Android 13+: READ_EXTERNAL_STORAGE grants nothing any more; reading other
            // apps' images (the screenshots we send to the PC) needs READ_MEDIA_IMAGES.
            if (p.equals(Manifest.permission.READ_EXTERNAL_STORAGE) && android.os.Build.VERSION.SDK_INT >= 33)
                p = "android.permission.READ_MEDIA_IMAGES";
            if (!p.isEmpty() && checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        if (!missing.isEmpty()) runOnUiThread(() -> requestPermissions(missing.toArray(new String[0]), 2));
    }

    public void speak(String text) {
        if (ttsReady) {
            tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, "rincast-ui");
            return;
        }
        // The engine takes a moment to start: say the latest thing once it's there.
        pendingSpeech = text;
        if (tts == null) {
            tts = new TextToSpeech(this, status -> {
                ttsReady = status == TextToSpeech.SUCCESS;
                if (ttsReady && pendingSpeech != null) tts.speak(pendingSpeech, TextToSpeech.QUEUE_FLUSH, null, "rincast-ui");
            });
        }
    }

    private static boolean isLocalHost(String host) {
        final String h = host.toLowerCase(java.util.Locale.ROOT);
        if (h.equals("localhost") || h.endsWith(".local") || h.equals("[::1]") || h.startsWith("[fe80:") || h.startsWith("[fd") || h.startsWith("[fc")) return true;
        final String[] p = h.split("\\.");
        if (p.length != 4) return false;
        try {
            final int a = Integer.parseInt(p[0]), b = Integer.parseInt(p[1]);
            return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /**
     * Blocking HTTP(S) request, called from a native worker thread.
     * Returns "<status>\n<body>"; status 0 means a network error.
     */
    public String httpRequest(String method, String url, String body, String bearer, int timeoutMs) {
        HttpURLConnection conn = null;
        try {
            // Sign-in tokens never travel unencrypted over the internet. Plain http is only
            // for a self-hosted server on the same network.
            final URL parsed = new URL(url);
            if (parsed.getProtocol().equals("http") && !isLocalHost(parsed.getHost())) {
                throw new java.io.IOException("This server needs an https:// address.");
            }
            conn = (HttpURLConnection) parsed.openConnection();
            conn.setRequestMethod(method);
            conn.setConnectTimeout(timeoutMs);
            conn.setReadTimeout(timeoutMs);
            conn.setRequestProperty("Accept", "application/json");
            conn.setRequestProperty("User-Agent", "RinCast-Headset/1.0");
            if (bearer != null && !bearer.isEmpty()) {
                conn.setRequestProperty("Authorization", "Bearer " + bearer);
            }
            if (body != null && !body.isEmpty()) {
                conn.setDoOutput(true);
                conn.setRequestProperty("Content-Type", "application/json");
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                conn.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream os = conn.getOutputStream()) {
                    os.write(bytes);
                }
            }
            int status = conn.getResponseCode();
            InputStream is = status >= 400 ? conn.getErrorStream() : conn.getInputStream();
            String text = "";
            if (is != null) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = is.read(buf)) > 0) out.write(buf, 0, n);
                is.close();
                text = out.toString("UTF-8");
            }
            return status + "\n" + text;
        } catch (Exception e) {
            return "0\n" + e.getClass().getSimpleName() + ": " + e.getMessage();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
