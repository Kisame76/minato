package io.github.kisame76.minato.test.servicesi;

import android.app.Application;
import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.os.HandlerThread;
import android.telephony.ims.ImsManager;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * Services bundle C, part 2 of the system services gap: what an app reads from NsdManager (discovery and
 * registration of a service on the local network), WifiP2pManager (Wi-Fi Direct) and ImsManager (IMS over a
 * subscription) on a phone without the hardware. The sections run on a thread of their own and wait for their
 * callbacks for a few seconds; each logs its lines on its own, so that one that fails does not hide the others; a
 * Throwable shows as its class name.
 */
public class ServicesIApplication extends Application {
    private static final String TAG = "services-i-test";

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread worker = new Thread(() -> {
            nsd();
            wifiP2p();
            ims();
        });
        worker.start();
        try {
            worker.join(60000);
        } catch (InterruptedException e) {
            Log.i(TAG, "interrupted");
        }
        Log.i(TAG, "done");
    }

    private static void failed(String section, Throwable t) {
        Log.i(TAG, section + ": " + t.getClass().getName());
    }

    private static void pause(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void nsd() {
        try {
            final NsdManager nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
            Log.i(TAG, "nsd manager: " + (nsd != null));
            final List<String> events = new ArrayList<>();
            final NsdManager.DiscoveryListener discovery = new NsdManager.DiscoveryListener() {
                @Override
                public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                    events.add("start failed " + errorCode);
                }

                @Override
                public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                    events.add("stop failed " + errorCode);
                }

                @Override
                public void onDiscoveryStarted(String serviceType) {
                    events.add("started " + serviceType);
                }

                @Override
                public void onDiscoveryStopped(String serviceType) {
                    events.add("stopped " + serviceType);
                }

                @Override
                public void onServiceFound(NsdServiceInfo serviceInfo) {
                    events.add("found");
                }

                @Override
                public void onServiceLost(NsdServiceInfo serviceInfo) {
                    events.add("lost");
                }
            };
            nsd.discoverServices("_minatotest._tcp", NsdManager.PROTOCOL_DNS_SD, discovery);
            pause(3000);
            Log.i(TAG, "nsd discovery events: " + events);
            events.clear();
            nsd.stopServiceDiscovery(discovery);
            pause(2000);
            Log.i(TAG, "nsd stop events: " + events);
            final List<String> registered = new ArrayList<>();
            final NsdServiceInfo info = new NsdServiceInfo();
            info.setServiceName("MinatoTest");
            info.setServiceType("_minatotest._tcp");
            info.setPort(47099);
            final NsdManager.RegistrationListener registration = new NsdManager.RegistrationListener() {
                @Override
                public void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    registered.add("registration failed " + errorCode);
                }

                @Override
                public void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    registered.add("unregistration failed " + errorCode);
                }

                @Override
                public void onServiceRegistered(NsdServiceInfo serviceInfo) {
                    registered.add("registered");
                }

                @Override
                public void onServiceUnregistered(NsdServiceInfo serviceInfo) {
                    registered.add("unregistered");
                }
            };
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, registration);
            pause(3000);
            Log.i(TAG, "nsd registration events: " + registered);
            registered.clear();
            nsd.unregisterService(registration);
            pause(2000);
            Log.i(TAG, "nsd unregistration events: " + registered);
        } catch (Throwable t) {
            failed("nsd", t);
        }
    }

    private void wifiP2p() {
        try {
            final WifiP2pManager p2p = (WifiP2pManager) getSystemService(Context.WIFI_P2P_SERVICE);
            Log.i(TAG, "wifi p2p manager: " + (p2p != null));
            final HandlerThread thread = new HandlerThread("p2p");
            thread.start();
            final WifiP2pManager.Channel channel = p2p.initialize(this, thread.getLooper(), null);
            Log.i(TAG, "wifi p2p channel: " + (channel != null));
            final List<String> answers = new ArrayList<>();
            p2p.discoverPeers(channel, new WifiP2pManager.ActionListener() {
                @Override
                public void onSuccess() {
                    answers.add("success");
                }

                @Override
                public void onFailure(int reason) {
                    answers.add("failure " + reason);
                }
            });
            pause(3000);
            Log.i(TAG, "wifi p2p discover peers: " + answers);
            thread.quitSafely();
        } catch (Throwable t) {
            failed("wifi p2p", t);
        }
    }

    private void ims() {
        try {
            final ImsManager ims = (ImsManager) getSystemService(Context.TELEPHONY_IMS_SERVICE);
            Log.i(TAG, "ims manager: " + (ims != null));
            try {
                Log.i(TAG, "ims mmtel manager: " + (ims.getImsMmTelManager(1) != null));
            } catch (Throwable t) {
                failed("ims mmtel manager", t);
            }
            try {
                Log.i(TAG, "ims rcs manager: " + (ims.getImsRcsManager(1) != null));
            } catch (Throwable t) {
                failed("ims rcs manager", t);
            }
        } catch (Throwable t) {
            failed("ims", t);
        }
    }
}
