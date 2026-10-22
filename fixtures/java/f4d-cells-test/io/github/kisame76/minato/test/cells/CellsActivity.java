package io.github.kisame76.minato.test.cells;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.telephony.AvailableNetworkInfo;
import android.telephony.CellIdentity;
import android.telephony.CellIdentityCdma;
import android.telephony.CellIdentityGsm;
import android.telephony.CellIdentityLte;
import android.telephony.CellIdentityNr;
import android.telephony.CellIdentityTdscdma;
import android.telephony.CellIdentityWcdma;
import android.telephony.CellInfo;
import android.telephony.CellInfoCdma;
import android.telephony.CellInfoGsm;
import android.telephony.CellInfoLte;
import android.telephony.CellInfoNr;
import android.telephony.CellInfoTdscdma;
import android.telephony.CellInfoWcdma;
import android.telephony.CellSignalStrength;
import android.telephony.CellSignalStrengthCdma;
import android.telephony.CellSignalStrengthGsm;
import android.telephony.CellSignalStrengthLte;
import android.telephony.CellSignalStrengthNr;
import android.telephony.CellSignalStrengthTdscdma;
import android.telephony.CellSignalStrengthWcdma;
import android.telephony.ClosedSubscriberGroupInfo;
import android.telephony.DisconnectCause;
import android.telephony.NeighboringCellInfo;
import android.telephony.RadioAccessSpecifier;
import android.telephony.SignalThresholdInfo;
import android.telephony.TelephonyManager;
import android.telephony.UiccPortInfo;
import android.telephony.VisualVoicemailSms;
import android.telephony.gsm.GsmCellLocation;
import android.util.Log;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The test of framework breadth batch F4d: the telephony cell family as an SDK reads it. Cells of every radio
 * technology are made through the public CREATOR of CellInfo from parcels in the layout AOSP writes (the cells have no
 * public constructors, and a phone without a SIM sees none), read back through their getters and tested with
 * instanceof, and the small data types around them are built through their public constructors. It runs on the main
 * thread when the window first has the focus and logs under the tag f4d-cells-test; tools/android-reference.sh takes
 * the log from Android 15, and tools/f4d-cells-check.sh compares Minato's with it.
 */
public class CellsActivity extends Activity {
    static final String TAG = "f4d-cells-test";
    // CellInfo.TYPE_*, hidden constants: the values of the cell types in a parcel
    private static final int TYPE_GSM = 1;
    private static final int TYPE_CDMA = 2;
    private static final int TYPE_LTE = 3;
    private static final int TYPE_WCDMA = 4;
    private static final int TYPE_TDSCDMA = 5;
    private static final int TYPE_NR = 6;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName());
            }
            log(line.toString());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            part("lte", this::lte);
            part("gsm", this::gsm);
            part("wcdma", this::wcdma);
            part("tdscdma", this::tdscdma);
            part("cdma", this::cdma);
            part("nr", this::nr);
            part("list", this::list);
            part("manager", this::manager);
            part("thresholds", this::thresholds);
            part("specifiers", this::specifiers);
            part("neighbors", this::neighbors);
            part("dataTypes", this::dataTypes);
            log("done");
            finish();
        });
    }

    /** CellInfo's common head: the type, registered, the time stamp and the connection status. */
    private static void head(Parcel p, int type, boolean registered, long timeStamp, int connection) {
        p.writeInt(type);
        p.writeInt(registered ? 1 : 0);
        p.writeLong(timeStamp);
        p.writeInt(connection);
    }

    /** CellIdentity's common head: the type, MCC, MNC and the two operator names. */
    private static void identityHead(Parcel p, int type, String mcc, String mnc, String longName, String shortName) {
        p.writeInt(type);
        p.writeString(mcc);
        p.writeString(mnc);
        p.writeString(longName);
        p.writeString(shortName);
    }

    private static CellInfo read(Parcel p) {
        p.setDataPosition(0);
        CellInfo info = CellInfo.CREATOR.createFromParcel(p);
        p.recycle();
        return info;
    }

    private static String common(CellInfo info) {
        CellIdentity id = info.getCellIdentity();
        CellSignalStrength strength = info.getCellSignalStrength();
        return info.getClass().getSimpleName() + " cell=" + (info instanceof CellInfo) + " registered="
                + info.isRegistered() + " connection=" + info.getCellConnectionStatus() + " time="
                + info.getTimestampMillis() + " identity=" + id.getClass().getSimpleName() + " operator="
                + id.getOperatorAlphaLong() + "/" + id.getOperatorAlphaShort() + " strength="
                + strength.getClass().getSimpleName() + " level=" + strength.getLevel() + " dbm=" + strength.getDbm()
                + " asu=" + strength.getAsuLevel() + " valid=" + isValid(strength);
    }

    private static boolean isValid(CellSignalStrength strength) {
        return strength.getLevel() != CellSignalStrength.SIGNAL_STRENGTH_NONE_OR_UNKNOWN;
    }

    private void lte() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_LTE, true, 1234L, CellInfo.CONNECTION_PRIMARY_SERVING);
        identityHead(p, TYPE_LTE, "262", "01", "Alpha Long", "Alpha");
        p.writeInt(123456);
        p.writeInt(77);
        p.writeInt(4711);
        p.writeInt(6300);
        p.writeIntArray(new int[] {3, 20});
        p.writeInt(10000);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeString("android.telephony.ClosedSubscriberGroupInfo");
        p.writeBoolean(true);
        p.writeString("home");
        p.writeInt(77);
        p.writeInt(-60);
        p.writeInt(-95);
        p.writeInt(-10);
        p.writeInt(20);
        p.writeInt(2);
        p.writeInt(11);
        p.writeInt(3);
        p.writeInt(3);
        p.writeBoolean(false);
        CellInfo info = read(p);
        log("lte: " + common(info) + " lte=" + (info instanceof CellInfoLte));
        CellIdentityLte id = ((CellInfoLte) info).getCellIdentity();
        log("lte identity: ci=" + id.getCi() + " pci=" + id.getPci() + " tac=" + id.getTac() + " earfcn="
                + id.getEarfcn() + " bands=" + Arrays.toString(id.getBands()) + " bandwidth=" + id.getBandwidth()
                + " mcc=" + id.getMccString() + " mnc=" + id.getMncString() + " plmn=" + id.getMobileNetworkOperator()
                + " additional=" + id.getAdditionalPlmns().size());
        ClosedSubscriberGroupInfo csg = id.getClosedSubscriberGroupInfo();
        log("lte csg: " + csg.getCsgIndicator() + " " + csg.getHomeNodebName() + " " + csg.getCsgIdentity());
        CellSignalStrengthLte s = ((CellInfoLte) info).getCellSignalStrength();
        log("lte strength: rssi=" + s.getRssi() + " rsrp=" + s.getRsrp() + " rsrq=" + s.getRsrq() + " rssnr="
                + s.getRssnr() + " cqiTable=" + s.getCqiTableIndex() + " cqi=" + s.getCqi() + " ta="
                + s.getTimingAdvance());
    }

    private void gsm() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_GSM, false, 99L, CellInfo.CONNECTION_NONE);
        identityHead(p, TYPE_GSM, "310", "260", "G Long", "G");
        p.writeInt(1000);
        p.writeInt(2000);
        p.writeInt(512);
        p.writeInt(17);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeInt(-70);
        p.writeInt(2);
        p.writeInt(1);
        p.writeInt(2);
        CellInfo info = read(p);
        log("gsm: " + common(info) + " gsm=" + (info instanceof CellInfoGsm));
        CellIdentityGsm id = ((CellInfoGsm) info).getCellIdentity();
        log("gsm identity: lac=" + id.getLac() + " cid=" + id.getCid() + " arfcn=" + id.getArfcn() + " bsic="
                + id.getBsic() + " mcc=" + id.getMccString() + " mnc=" + id.getMncString());
        CellSignalStrengthGsm s = ((CellInfoGsm) info).getCellSignalStrength();
        log("gsm strength: rssi=" + s.getRssi() + " ber=" + s.getBitErrorRate() + " ta=" + s.getTimingAdvance());
    }

    private void wcdma() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_WCDMA, true, 5L, CellInfo.CONNECTION_SECONDARY_SERVING);
        identityHead(p, TYPE_WCDMA, "001", "01", null, null);
        p.writeInt(3000);
        p.writeInt(4000);
        p.writeInt(55);
        p.writeInt(10562);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeParcelable(null, 0);
        p.writeInt(-80);
        p.writeInt(1);
        p.writeInt(-90);
        p.writeInt(-8);
        p.writeInt(2);
        CellInfo info = read(p);
        log("wcdma: " + common(info) + " wcdma=" + (info instanceof CellInfoWcdma));
        CellIdentityWcdma id = ((CellInfoWcdma) info).getCellIdentity();
        log("wcdma identity: lac=" + id.getLac() + " cid=" + id.getCid() + " psc=" + id.getPsc() + " uarfcn="
                + id.getUarfcn() + " csg=" + id.getClosedSubscriberGroupInfo());
        CellSignalStrengthWcdma s = ((CellInfoWcdma) info).getCellSignalStrength();
        log("wcdma strength: ecNo=" + s.getEcNo());
    }

    private void tdscdma() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_TDSCDMA, true, 6L, CellInfo.CONNECTION_UNKNOWN);
        identityHead(p, TYPE_TDSCDMA, "460", "00", "China", "CN");
        p.writeInt(5000);
        p.writeInt(6000);
        p.writeInt(33);
        p.writeInt(10054);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeParcelable(null, 0);
        p.writeInt(-85);
        p.writeInt(3);
        p.writeInt(-100);
        p.writeInt(1);
        CellInfo info = read(p);
        log("tdscdma: " + common(info) + " tdscdma=" + (info instanceof CellInfoTdscdma));
        CellIdentityTdscdma id = ((CellInfoTdscdma) info).getCellIdentity();
        log("tdscdma identity: lac=" + id.getLac() + " cid=" + id.getCid() + " cpid=" + id.getCpid() + " uarfcn="
                + id.getUarfcn());
        CellSignalStrengthTdscdma s = ((CellInfoTdscdma) info).getCellSignalStrength();
        log("tdscdma strength: rscp=" + s.getRscp());
    }

    private void cdma() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_CDMA, false, 7L, CellInfo.CONNECTION_NONE);
        identityHead(p, TYPE_CDMA, null, null, "Cdma", "C");
        p.writeInt(10);
        p.writeInt(20);
        p.writeInt(30);
        p.writeInt(100);
        p.writeInt(200);
        p.writeInt(-75);
        p.writeInt(-90);
        p.writeInt(-80);
        p.writeInt(-100);
        p.writeInt(5);
        p.writeInt(2);
        CellInfo info = read(p);
        log("cdma: " + common(info) + " cdma=" + (info instanceof CellInfoCdma));
        CellIdentityCdma id = ((CellInfoCdma) info).getCellIdentity();
        log("cdma identity: network=" + id.getNetworkId() + " system=" + id.getSystemId() + " base="
                + id.getBasestationId() + " lon=" + id.getLongitude() + " lat=" + id.getLatitude());
        CellSignalStrengthCdma s = ((CellInfoCdma) info).getCellSignalStrength();
        log("cdma strength: cdmaDbm=" + s.getCdmaDbm() + " cdmaEcio=" + s.getCdmaEcio() + " evdoDbm="
                + s.getEvdoDbm() + " evdoSnr=" + s.getEvdoSnr());
    }

    private void nr() {
        Parcel p = Parcel.obtain();
        head(p, TYPE_NR, true, 8L, CellInfo.CONNECTION_PRIMARY_SERVING);
        identityHead(p, TYPE_NR, "262", "02", "Nr Long", "Nr");
        p.writeInt(500);
        p.writeInt(12345);
        p.writeInt(632628);
        p.writeIntArray(new int[] {78});
        p.writeLong(123456789012L);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeInt(-95);
        p.writeInt(-11);
        p.writeInt(15);
        p.writeInt(1);
        p.writeList(new ArrayList<Integer>(Arrays.asList(1, 2, 3)));
        p.writeInt(-100);
        p.writeInt(-12);
        p.writeInt(10);
        p.writeInt(3);
        p.writeInt(4);
        CellInfo info = read(p);
        log("nr: " + common(info) + " nr=" + (info instanceof CellInfoNr));
        CellIdentityNr id = (CellIdentityNr) info.getCellIdentity();
        log("nr identity: pci=" + id.getPci() + " tac=" + id.getTac() + " nrarfcn=" + id.getNrarfcn() + " bands="
                + Arrays.toString(id.getBands()) + " nci=" + id.getNci());
        CellSignalStrengthNr s = (CellSignalStrengthNr) info.getCellSignalStrength();
        log("nr strength: csiRsrp=" + s.getCsiRsrp() + " csiSinr=" + s.getCsiSinr() + " ssRsrp=" + s.getSsRsrp()
                + " ssSinr=" + s.getSsSinr() + " csiCqi=" + s.getCsiCqiReport());
    }

    /** What an SDK does with a list of cells: tests each with instanceof. */
    private void list() {
        List<CellInfo> cells = new ArrayList<>();
        Parcel p = Parcel.obtain();
        head(p, TYPE_GSM, true, 1L, CellInfo.CONNECTION_PRIMARY_SERVING);
        identityHead(p, TYPE_GSM, "001", "01", null, null);
        p.writeInt(1);
        p.writeInt(2);
        p.writeInt(3);
        p.writeInt(4);
        p.writeInt(0); // an empty ArraySet: writeArraySet is hidden
        p.writeInt(-60);
        p.writeInt(0);
        p.writeInt(0);
        p.writeInt(4);
        cells.add(read(p));
        int gsm = 0;
        int lte = 0;
        int other = 0;
        for (CellInfo cell : cells) {
            if (cell instanceof CellInfoLte) {
                lte++;
            } else if (cell instanceof CellInfoGsm) {
                gsm++;
            } else {
                other++;
            }
        }
        log("list: gsm=" + gsm + " lte=" + lte + " other=" + other);
        p = Parcel.obtain();
        p.writeInt(99);
        p.setDataPosition(0);
        try {
            CellInfo.CREATOR.createFromParcel(p);
        } catch (RuntimeException e) {
            log("bad type: " + e.getClass().getName() + ": " + e.getMessage());
        }
        p.recycle();
        log("newArray: " + CellInfo.CREATOR.newArray(2).length + " " + CellInfoLte.CREATOR.newArray(3).length);
    }

    /** The lists TelephonyManager returns on a phone without a SIM, and the location permission an app lacks. */
    private void manager() {
        TelephonyManager tm = getSystemService(TelephonyManager.class);
        try {
            List<CellInfo> all = tm.getAllCellInfo();
            log("allCellInfo: " + (all == null ? "null" : "size=" + all.size()));
        } catch (SecurityException e) {
            log("allCellInfo: SecurityException");
        }
    }

    private void thresholds() {
        SignalThresholdInfo info = new SignalThresholdInfo.Builder()
                .setRadioAccessNetworkType(3)
                .setSignalMeasurementType(SignalThresholdInfo.SIGNAL_MEASUREMENT_TYPE_RSRP)
                .setThresholds(new int[] {-120, -110, -100, -90})
                .setHysteresisDb(2)
                .build();
        log("threshold: network=" + info.getRadioAccessNetworkType() + " type=" + info.getSignalMeasurementType()
                + " values=" + Arrays.toString(info.getThresholds()) + " hysteresisDb=" + info.getHysteresisDb()
                + " min=" + SignalThresholdInfo.getMinimumNumberOfThresholdsAllowed() + " max="
                + SignalThresholdInfo.getMaximumNumberOfThresholdsAllowed());
        Parcel p = Parcel.obtain();
        info.writeToParcel(p, 0);
        p.setDataPosition(0);
        SignalThresholdInfo back = SignalThresholdInfo.CREATOR.createFromParcel(p);
        p.recycle();
        log("threshold parcel: " + back.equals(info) + " " + Arrays.toString(back.getThresholds()));
        try {
            new SignalThresholdInfo.Builder().setThresholds(new int[0]);
        } catch (IllegalArgumentException e) {
            log("threshold empty: IllegalArgumentException");
        }
    }

    private void specifiers() {
        RadioAccessSpecifier specifier = new RadioAccessSpecifier(3, new int[] {3, 20}, new int[] {6300});
        log("specifier: network=" + specifier.getRadioAccessNetwork() + " bands="
                + Arrays.toString(specifier.getBands()) + " channels=" + Arrays.toString(specifier.getChannels()));
        Parcel p = Parcel.obtain();
        specifier.writeToParcel(p, 0);
        p.setDataPosition(0);
        RadioAccessSpecifier back = RadioAccessSpecifier.CREATOR.createFromParcel(p);
        p.recycle();
        log("specifier parcel: " + back.equals(specifier) + " " + back.hashCode() + " " + specifier.hashCode());
        List<String> plmns = new ArrayList<>(Arrays.asList("26201", "26202"));
        List<RadioAccessSpecifier> specifiers = new ArrayList<>();
        specifiers.add(specifier);
        AvailableNetworkInfo available = new AvailableNetworkInfo.Builder(1)
                .setPriority(AvailableNetworkInfo.PRIORITY_MED)
                .setMccMncs(plmns)
                .setRadioAccessSpecifiers(specifiers)
                .build();
        log("available: sub=" + available.getSubId() + " priority=" + available.getPriority() + " plmns="
                + available.getMccMncs() + " specifiers=" + available.getRadioAccessSpecifiers().size());
    }

    @SuppressWarnings("deprecation")
    private void neighbors() {
        NeighboringCellInfo cell = new NeighboringCellInfo(20, "1F4", TelephonyManager.NETWORK_TYPE_UMTS);
        log("neighbor: rssi=" + cell.getRssi() + " cid=" + cell.getCid() + " lac=" + cell.getLac() + " psc="
                + cell.getPsc() + " type=" + cell.getNetworkType() + " " + new NeighboringCellInfo(30, 5).getCid());
        NeighboringCellInfo none = new NeighboringCellInfo();
        log("neighbor empty: rssi=" + none.getRssi() + " cid=" + none.getCid());
        GsmCellLocation location = new GsmCellLocation();
        log("gsmCellLocation: lac=" + location.getLac() + " cid=" + location.getCid() + " psc=" + location.getPsc());
        location.setLacAndCid(11, 22);
        log("gsmCellLocation set: lac=" + location.getLac() + " cid=" + location.getCid());
    }

    private void dataTypes() {
        Parcel p = Parcel.obtain();
        // writeString8 is hidden: its length, the UTF-8 bytes of "8901" and the NUL, padded to four bytes
        p.writeInt(4);
        p.writeInt(0x31303938);
        p.writeInt(0);
        p.writeInt(0);
        p.writeInt(1);
        p.writeBoolean(true);
        p.setDataPosition(0);
        UiccPortInfo port = UiccPortInfo.CREATOR.createFromParcel(p);
        p.recycle();
        log("uiccPort: port=" + port.getPortIndex() + " slot=" + port.getLogicalSlotIndex() + " active="
                + port.isActive() + " redacted=" + UiccPortInfo.ICCID_REDACTED);
        p = Parcel.obtain();
        p.writeParcelable(null, 0);
        p.writeString("//VVM");
        Bundle fields = new Bundle();
        fields.putString("st", "N");
        p.writeBundle(fields);
        p.writeString("body");
        p.setDataPosition(0);
        VisualVoicemailSms sms = VisualVoicemailSms.CREATOR.createFromParcel(p);
        p.recycle();
        log("visualVoicemailSms: " + sms.getPrefix() + " " + sms.getFields().getString("st") + " "
                + sms.getMessageBody() + " " + sms.getPhoneAccountHandle());
        log("disconnectCause: " + DisconnectCause.NORMAL + " " + DisconnectCause.BUSY + " "
                + DisconnectCause.CALL_BARRED + " " + DisconnectCause.ALREADY_DIALING);
    }
}
