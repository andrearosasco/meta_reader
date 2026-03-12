package com.example.metareader;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;

import java.net.InetAddress;

final class QuestTeleopDiscoveryManager {
    private static final String SERVICE_TYPE = "_quest-teleop._udp.";

    private final NsdManager nsdManager;
    private final WifiManager.MulticastLock multicastLock;

    private boolean shouldRun;
    private boolean discoveryActive;
    private NsdManager.DiscoveryListener discoveryListener;

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
        stopDiscovery();
        releaseMulticastLock();
    }

    private void beginDiscovery() {
        if (!shouldRun || discoveryActive || nsdManager == null) {
            return;
        }

        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                discoveryActive = false;
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                discoveryActive = false;
            }

            @Override
            public void onDiscoveryStarted(String serviceType) {
                discoveryActive = true;
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
                discoveryActive = false;
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                if (!SERVICE_TYPE.equals(serviceInfo.getServiceType())) {
                    return;
                }

                if (serviceInfo.getServiceName() == null) {
                    return;
                }

                resolveService(serviceInfo);
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                MetaReaderActivity.nativeOnServiceLost();
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
        discoveryActive = false;
        discoveryListener = null;
    }

    private void resolveService(NsdServiceInfo serviceInfo) {
        if (serviceInfo.getServiceName() == null || nsdManager == null) {
            return;
        }

        nsdManager.resolveService(serviceInfo, new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo failedServiceInfo, int errorCode) {
            }

            @Override
            public void onServiceResolved(NsdServiceInfo resolvedServiceInfo) {
                InetAddress hostAddress = resolvedServiceInfo.getHost();
                String hostText = hostAddress != null ? hostAddress.getHostAddress() : "UNAVAILABLE";
                MetaReaderActivity.nativeOnServiceResolved(hostText, resolvedServiceInfo.getPort());
            }
        });
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