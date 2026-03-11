package com.example.metareader;

import android.app.NativeActivity;
import android.os.Bundle;

public final class MetaReaderActivity extends NativeActivity {
    static {
        System.loadLibrary("meta_reader");
    }

    private QuestTeleopDiscoveryManager discoveryManager;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        discoveryManager = new QuestTeleopDiscoveryManager(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (discoveryManager != null) {
            discoveryManager.start();
        }
    }

    @Override
    protected void onPause() {
        if (discoveryManager != null) {
            discoveryManager.stop();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (discoveryManager != null) {
            discoveryManager.stop();
            discoveryManager = null;
        }
        super.onDestroy();
    }

    static native void nativeOnDiscoveryState(String state, String detail);
    static native void nativeOnServiceResolved(String serviceName, String host, int port, String txtSummary);
    static native void nativeOnServiceLost(String serviceName);
}