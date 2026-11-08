package io.github.kisame76.minato.test.camerametadata;

import android.app.Application;
import android.content.Context;
import android.graphics.Rect;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.os.Parcel;
import android.util.Log;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.util.SizeF;

import java.util.Arrays;

/**
 * ADR-0073 step a1's test of camera2's metadata: CameraMetadataNative's natives over libcamera_metadata, in Minato
 * only, as the class is hidden API. Every value is set through a public CameraCharacteristics key and read back
 * through the marshalers, so each line is what AOSP's Java makes of the bytes the natives keep:
 * - an empty object; one key of each marshaled type (Integer, Byte, Long, Boolean, float[], Range, Range[], Size,
 *   SizeF, Rect, Rational); more keys than the first buffer holds, and a value that grows in place;
 * - a copy, an update of one object from another, a swap by move, a key removed by null;
 * - a key's tag and type from its name, and a name with no tag;
 * - a Parcel's round trip and the dump.
 * It logs with the tag camera-metadata-test and ends with "done".
 */
public class MetadataApplication extends Application {
    private static final String TAG = "camera-metadata-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            run();
        } catch (Throwable error) {
            Log.i(TAG, "failed: " + error);
        }
        Log.i(TAG, "done");
    }

    private static void run() {
        CameraMetadataNative metadata = new CameraMetadataNative();
        Log.i(TAG, "empty: " + metadata.isEmpty() + ", entries " + metadata.getEntryCount());

        metadata.set(CameraCharacteristics.LENS_FACING, CameraCharacteristics.LENS_FACING_BACK);
        metadata.set(CameraCharacteristics.REQUEST_PIPELINE_MAX_DEPTH, (byte) 8);
        metadata.set(CameraCharacteristics.SENSOR_INFO_MAX_FRAME_DURATION, 333333333L);
        metadata.set(CameraCharacteristics.FLASH_INFO_AVAILABLE, true);
        metadata.set(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES, new float[] {1.78f});
        metadata.set(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE, new Range<>(13611L, 1000000000L));
        metadata.set(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                new Range[] {new Range<>(15, 30), new Range<>(30, 30)});
        metadata.set(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE, new Size(4032, 3024));
        metadata.set(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE, new SizeF(5.76f, 4.32f));
        metadata.set(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE, new Rect(0, 0, 4032, 3024));
        metadata.set(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP, new Rational(1, 6));
        Log.i(TAG, "entries after 11 keys: " + metadata.getEntryCount() + ", empty " + metadata.isEmpty());
        show("set", metadata);

        // More keys than the first buffer's 10 entries, then a value that needs more room than its own.
        metadata.set(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS, new float[] {4.25f});
        metadata.set(CameraCharacteristics.SENSOR_ORIENTATION, 90);
        metadata.set(CameraCharacteristics.STATISTICS_INFO_MAX_FACE_COUNT, 10);
        metadata.set(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES, new float[] {1.78f, 2.2f, 2.8f, 4.0f});
        Log.i(TAG, "entries after 14 keys: " + metadata.getEntryCount());
        Log.i(TAG, "apertures grown: "
                + Arrays.toString(metadata.get(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES)));

        CameraMetadataNative copy = new CameraMetadataNative(metadata);
        copy.set(CameraCharacteristics.SENSOR_ORIENTATION, 270);
        Log.i(TAG, "copy: entries " + copy.getEntryCount() + ", orientation " + copy.get(
                CameraCharacteristics.SENSOR_ORIENTATION) + ", the original's " + metadata.get(
                CameraCharacteristics.SENSOR_ORIENTATION));

        CameraMetadataNative small = new CameraMetadataNative();
        small.set(CameraCharacteristics.LENS_FACING, CameraCharacteristics.LENS_FACING_FRONT);
        small.set(CameraCharacteristics.SENSOR_ORIENTATION, 180);
        CameraMetadataNative updated = new CameraMetadataNative(metadata);
        CameraMetadataNative.update(updated, small);
        Log.i(TAG, "update: facing " + updated.get(CameraCharacteristics.LENS_FACING) + ", orientation "
                + updated.get(CameraCharacteristics.SENSOR_ORIENTATION) + ", entries " + updated.getEntryCount());

        CameraMetadataNative moved = CameraMetadataNative.move(small);
        Log.i(TAG, "move: facing " + moved.get(CameraCharacteristics.LENS_FACING) + ", the source empty "
                + small.isEmpty());

        copy.set(CameraCharacteristics.SENSOR_ORIENTATION, null);
        Log.i(TAG, "removed: orientation " + copy.get(CameraCharacteristics.SENSOR_ORIENTATION) + ", entries "
                + copy.getEntryCount());

        Log.i(TAG, String.format("tag of android.lens.facing 0x%x, type %d",
                CameraMetadataNative.getTag("android.lens.facing"),
                CameraMetadataNative.getNativeType(CameraMetadataNative.getTag("android.lens.facing"),
                        Long.MAX_VALUE)));
        Log.i(TAG, String.format("tag of android.sensor.info.pixelArraySize 0x%x",
                CameraMetadataNative.getTag("android.sensor.info.pixelArraySize")));
        try {
            CameraMetadataNative.getTag("android.lens.noSuchKey");
            Log.i(TAG, "no such key: a tag");
        } catch (IllegalArgumentException error) {
            Log.i(TAG, "no such key: " + error.getMessage());
        }

        Parcel parcel = Parcel.obtain();
        metadata.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        CameraMetadataNative read = CameraMetadataNative.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Log.i(TAG, "parcel: entries " + read.getEntryCount());
        show("parcel", read);

        metadata.dumpToLog();
        Log.i(TAG, "dumped");

    }

    private static void show(String what, CameraMetadataNative metadata) {
        Log.i(TAG, what + ": facing " + metadata.get(CameraCharacteristics.LENS_FACING)
                + ", pipeline depth " + metadata.get(CameraCharacteristics.REQUEST_PIPELINE_MAX_DEPTH)
                + ", max frame duration " + metadata.get(CameraCharacteristics.SENSOR_INFO_MAX_FRAME_DURATION)
                + ", flash " + metadata.get(CameraCharacteristics.FLASH_INFO_AVAILABLE));
        Log.i(TAG, what + ": apertures " + Arrays.toString(
                metadata.get(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES)) + ", exposure "
                + metadata.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) + ", fps "
                + Arrays.toString(metadata.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)));
        Log.i(TAG, what + ": pixel array " + metadata.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
                + ", physical " + metadata.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) + ", active "
                + metadata.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) + ", step "
                + metadata.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP));
    }
}
