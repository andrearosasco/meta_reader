package com.example.metareader;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;

import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.TreeMap;

final class QuestTeleopDiscoveryManager {
    private static final String SERVICE_TYPE = "_quest-teleop._udp.";
    private static final long RESTART_DELAY_MS = 1500L;

    private final NsdManager nsdManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final WifiManager.MulticastLock multicastLock;

    private boolean shouldRun;
    private boolean discoveryActive;
    private NsdManager.DiscoveryListener discoveryListener;
    private String activeResolutionName;
    private String resolvedServiceName;

    QuestTeleopDiscoveryManager(Context context) {
        nsdManager = context.getSystemService(NsdManager.class);

        WifiManager wifiManager = context.getApplicationContext().getSystemService(WifiManager.class);
        if (wifiManager != null) {
            multicastLock = wifiManager.createMulticastLock("MetaReaderNsd");
            multicastLock.setReferenceCounted(false);
        } else {
            multicastLock = null;
        }
    }

    void start() {
        shouldRun = true;
        acquireMulticastLock();
        beginDiscovery();
    }

    void stop() {
        shouldRun = false;
        mainHandler.removeCallbacksAndMessages(null);
        stopDiscovery();
        releaseMulticastLock();
        activeResolutionName = null;
        resolvedServiceName = null;
    }

    private void beginDiscovery() {
        if (!shouldRun || discoveryActive || nsdManager == null) {
            return;
        }

        MetaReaderActivity.nativeOnDiscoveryState("DISCOVERING", "STARTING");
        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                MetaReaderActivity.nativeOnDiscoveryState("DISCOVERY ERR", "START " + errorCode);
                discoveryActive = false;
                scheduleRestart();
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                MetaReaderActivity.nativeOnDiscoveryState("DISCOVERY ERR", "STOP " + errorCode);
                discoveryActive = false;
                scheduleRestart();
            }

            @Override
            public void onDiscoveryStarted(String serviceType) {
                discoveryActive = true;
                MetaReaderActivity.nativeOnDiscoveryState("DISCOVERING", "RUNNING");
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
                discoveryActive = false;
                MetaReaderActivity.nativeOnDiscoveryState("DISCOVERING", "STOPPED");
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                if (!SERVICE_TYPE.equals(serviceInfo.getServiceType())) {
                    return;
                }

                final String serviceName = serviceInfo.getServiceName();
                if (serviceName == null || serviceName.equals(activeResolutionName) || serviceName.equals(resolvedServiceName)) {
                    return;
                }

                resolveService(serviceInfo);
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                final String serviceName = serviceInfo.getServiceName();
                if (serviceName != null && serviceName.equals(resolvedServiceName)) {
                    resolvedServiceName = null;
                    MetaReaderActivity.nativeOnServiceLost(serviceName);
                }
            }
        };

        nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
    }

    private void stopDiscovery() {
        if (!discoveryActive || discoveryListener == null || nsdManager == null) {
            discoveryActive = false;
            discoveryListener = null;
            return;
        }

        try {
            nsdManager.stopServiceDiscovery(discoveryListener);
        } catch (IllegalArgumentException ignored) {
            discoveryActive = false;
        }
        discoveryListener = null;
    }

    private void resolveService(NsdServiceInfo serviceInfo) {
        final String serviceName = serviceInfo.getServiceName();
        if (serviceName == null || nsdManager == null) {
            return;
        }

        activeResolutionName = serviceName;
        nsdManager.resolveService(serviceInfo, new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo failedServiceInfo, int errorCode) {
                activeResolutionName = null;
                MetaReaderActivity.nativeOnDiscoveryState("RESOLVE ERR", serviceName + ' ' + errorCode);
            }

            @Override
            public void onServiceResolved(NsdServiceInfo resolvedServiceInfo) {
                activeResolutionName = null;
                resolvedServiceName = resolvedServiceInfo.getServiceName();

                InetAddress hostAddress = resolvedServiceInfo.getHost();
                String hostText = hostAddress != null ? hostAddress.getHostAddress() : "UNAVAILABLE";
                MetaReaderActivity.nativeOnServiceResolved(
                    resolvedServiceInfo.getServiceName(),
                    hostText,
                    resolvedServiceInfo.getPort(),
                    buildTxtSummary(resolvedServiceInfo));
            }
        });
    }

    private String buildTxtSummary(NsdServiceInfo serviceInfo) {
        Map<String, byte[]> attributes = serviceInfo.getAttributes();
        if (attributes == null || attributes.isEmpty()) {
            return "";
        }

        TreeMap<String, byte[]> sortedAttributes = new TreeMap<>(attributes);
        StringBuilder summary = new StringBuilder();
        for (Map.Entry<String, byte[]> entry : sortedAttributes.entrySet()) {
            if (summary.length() > 0) {
                summary.append(", ");
            }

            summary.append(entry.getKey()).append('=');
            byte[] valueBytes = entry.getValue();
            if (valueBytes != null) {
                summary.append(new String(valueBytes, StandardCharsets.UTF_8));
            }
        }
        return summary.toString();
    }

    private void scheduleRestart() {
        if (!shouldRun) {
            return;
        }

        mainHandler.removeCallbacksAndMessages(null);
        mainHandler.postDelayed(this::beginDiscovery, RESTART_DELAY_MS);
    }

    private void acquireMulticastLock() {
        if (multicastLock != null && !multicastLock.isHeld()) {
            multicastLock.acquire();
        }
    }

    private void releaseMulticastLock() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
    }
}