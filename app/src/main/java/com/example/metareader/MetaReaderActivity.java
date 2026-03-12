package com.example.metareader;

import android.content.pm.PackageManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbManager;
import android.app.NativeActivity;
import android.os.Bundle;
import android.util.Log;

import java.lang.reflect.Method;

public final class MetaReaderActivity extends NativeActivity {
    private static final String LOG_TAG = "MetaReaderXR";
    private static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE";
    private static final String HAND_TRACKING_PERMISSION = "com.oculus.permission.HAND_TRACKING";
    private static final int HAND_TRACKING_PERMISSION_REQUEST_CODE = 1001;
    private static final int WIRED_TCP_PORT = 5005;

    private final BroadcastReceiver usbStateReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null || !ACTION_USB_STATE.equals(intent.getAction())) {
                return;
            }

            applyTransportMode(intent);
        }
    };

    static {
        System.loadLibrary("meta_reader");
    }

    private QuestTeleopDiscoveryManager discoveryManager;
    private boolean usbReceiverRegistered;
    private boolean resumed;
    private Boolean wiredModeEnabled;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        discoveryManager = new QuestTeleopDiscoveryManager(this);
        ensureHandTrackingPermission();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        ensureHandTrackingPermission();
        registerUsbStateReceiver();
        refreshTransportMode();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != HAND_TRACKING_PERMISSION_REQUEST_CODE) {
            return;
        }

        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        Log.i(LOG_TAG, "Hand tracking permission result: granted=" + granted);
    }

    @Override
    protected void onPause() {
        resumed = false;
        stopWirelessDiscovery();
        unregisterUsbStateReceiver();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        unregisterUsbStateReceiver();
        if (discoveryManager != null) {
            discoveryManager.stop();
            discoveryManager = null;
        }
        super.onDestroy();
    }

    private void registerUsbStateReceiver() {
        if (usbReceiverRegistered) {
            return;
        }

        registerReceiver(usbStateReceiver, new IntentFilter(ACTION_USB_STATE));
        usbReceiverRegistered = true;
    }

    private void unregisterUsbStateReceiver() {
        if (!usbReceiverRegistered) {
            return;
        }

        unregisterReceiver(usbStateReceiver);
        usbReceiverRegistered = false;
    }

    private void refreshTransportMode() {
        Intent usbState = registerReceiver(null, new IntentFilter(ACTION_USB_STATE));
        applyTransportMode(usbState);
    }

    private void applyTransportMode(Intent usbState) {
        boolean wiredMode = isAdbUsbConnected(usbState);
        String detail = describeUsbState(usbState);
        if (wiredModeEnabled != null && wiredModeEnabled == wiredMode) {
            nativeOnTransportModeChanged(wiredMode ? "WIRED" : "WIRELESS", detail, WIRED_TCP_PORT);
            return;
        }

        wiredModeEnabled = wiredMode;
        nativeOnTransportModeChanged(wiredMode ? "WIRED" : "WIRELESS", detail, WIRED_TCP_PORT);

        if (wiredMode) {
            stopWirelessDiscovery();
            Log.i(LOG_TAG, "Transport mode selected: wired ADB reverse TCP via 127.0.0.1:" + WIRED_TCP_PORT + " (" + detail + ")");
            return;
        }

        if (resumed && discoveryManager != null) {
            discoveryManager.start();
        }
        Log.i(LOG_TAG, "Transport mode selected: wireless Avahi DNS-SD discovery (" + detail + ")");
    }

    private void stopWirelessDiscovery() {
        if (discoveryManager != null) {
            discoveryManager.stop();
        }
    }

    private void ensureHandTrackingPermission() {
        if (checkSelfPermission(HAND_TRACKING_PERMISSION) == PackageManager.PERMISSION_GRANTED) {
            return;
        }

        Log.i(LOG_TAG, "Requesting hand tracking permission.");
        requestPermissions(new String[] {HAND_TRACKING_PERMISSION}, HAND_TRACKING_PERMISSION_REQUEST_CODE);
    }

    private static boolean isAdbUsbConnected(Intent usbState) {
        if (usbState == null) {
            return false;
        }

        boolean connected = usbState.getBooleanExtra("connected", false);
        boolean configured = usbState.getBooleanExtra("configured", false);
        boolean adbEnabled = usbState.getBooleanExtra("adb", false);
        if (connected && configured && adbEnabled) {
            return true;
        }

        String usbFunctions = getSystemUsbState();
        return connected && configured && usbFunctions.contains("adb");
    }

    private static String describeUsbState(Intent usbState) {
        if (usbState == null) {
            return "usb_state=unavailable";
        }

        return "connected=" + usbState.getBooleanExtra("connected", false)
            + ", configured=" + usbState.getBooleanExtra("configured", false)
            + ", adb=" + usbState.getBooleanExtra("adb", false)
            + ", sys.usb.state=" + getSystemUsbState();
    }

    private static String getSystemUsbState() {
        try {
            Class<?> systemProperties = Class.forName("android.os.SystemProperties");
            Method getMethod = systemProperties.getMethod("get", String.class, String.class);
            Object value = getMethod.invoke(null, "sys.usb.state", "");
            return value instanceof String ? (String) value : "";
        } catch (ReflectiveOperationException ignored) {
            return "";
        }
    }

    static native void nativeOnTransportModeChanged(String mode, String detail, int wiredPort);
    static native void nativeOnDiscoveryState(String state, String detail);
    static native void nativeOnServiceResolved(String serviceName, String host, int port, String txtSummary);
    static native void nativeOnServiceLost(String serviceName);
}