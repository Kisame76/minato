package io.github.kisame76.minato.test.speech;

import android.app.Application;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import android.util.Log;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The speech recognition test (group 3 of the framework breadth): the shapes of SpeechRecognizer, RecognitionListener
 * and RecognizerIntent, what a phone without a recognition service answers (availability, the recognizer the factory
 * makes, the activity intent) and what startListening calls back, in an app without RECORD_AUDIO.
 */
public class SpeechApplication extends Application {
    private static final String TAG = "speech-test";
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private boolean mDone;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            shapes();
            availability();
            listen();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            finish();
        }
    }

    private void finish() {
        if (mDone) return;
        mDone = true;
        Log.i(TAG, "done");
    }

    private static void shapes() {
        Log.i(TAG, "listener methods: " + names(RecognitionListener.class, null));
        // The API's methods only: AOSP's source also has a hidden test factory, which Android 15's build lacks.
        Log.i(TAG, "SpeechRecognizer methods: " + names(SpeechRecognizer.class, java.util.Arrays.asList("cancel",
            "checkRecognitionSupport", "createOnDeviceSpeechRecognizer", "createSpeechRecognizer", "destroy",
            "isOnDeviceRecognitionAvailable", "isRecognitionAvailable", "setRecognitionListener", "startListening",
            "stopListening", "triggerModelDownload")));
        StringBuilder errors = new StringBuilder();
        for (java.lang.reflect.Field field : SpeechRecognizer.class.getFields()) {
            if (field.getName().startsWith("ERROR_") || field.getName().startsWith("RESULTS_")
                || field.getName().startsWith("CONFIDENCE_")) {
                try {
                    errors.append(' ').append(field.getName()).append('=').append(field.get(null));
                } catch (IllegalAccessException e) {
                    errors.append(" ?");
                }
            }
        }
        Log.i(TAG, "SpeechRecognizer constants:" + errors);
        Log.i(TAG, "RecognizerIntent actions: " + RecognizerIntent.ACTION_RECOGNIZE_SPEECH + ", "
            + RecognizerIntent.ACTION_WEB_SEARCH + ", " + RecognizerIntent.ACTION_VOICE_SEARCH_HANDS_FREE);
        Log.i(TAG, "RecognizerIntent extras: " + RecognizerIntent.EXTRA_LANGUAGE_MODEL + ", "
            + RecognizerIntent.EXTRA_PARTIAL_RESULTS + ", " + RecognizerIntent.EXTRA_RESULTS + ", "
            + RecognizerIntent.EXTRA_MAX_RESULTS + ", " + RecognizerIntent.EXTRA_PROMPT);
        Log.i(TAG, "language models: " + RecognizerIntent.LANGUAGE_MODEL_FREE_FORM + ", "
            + RecognizerIntent.LANGUAGE_MODEL_WEB_SEARCH);
    }

    private static String names(Class<?> type, List<String> allowed) {
        List<String> names = new ArrayList<>();
        for (Method method : type.getDeclaredMethods()) {
            if (Modifier.isPublic(method.getModifiers()) && !method.isSynthetic()
                && !method.getName().startsWith("lambda$") && (allowed == null || allowed.contains(method.getName()))) {
                names.add(method.getName());
            }
        }
        Collections.sort(names);
        return names.toString();
    }

    private void availability() {
        Log.i(TAG, "isRecognitionAvailable: " + SpeechRecognizer.isRecognitionAvailable(this));
        Log.i(TAG, "isOnDeviceRecognitionAvailable: " + SpeechRecognizer.isOnDeviceRecognitionAvailable(this));
        Intent intent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        Log.i(TAG, "recognizer activity: " + (intent.resolveActivity(getPackageManager()) != null));
        try {
            startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            Log.i(TAG, "startActivity: started");
        } catch (ActivityNotFoundException e) {
            Log.i(TAG, "startActivity: ActivityNotFoundException");
        }
        SpeechRecognizer recognizer = SpeechRecognizer.createSpeechRecognizer(this);
        Log.i(TAG, "createSpeechRecognizer: " + (recognizer != null));
        if (recognizer != null) recognizer.destroy();
        SpeechRecognizer onDevice = null;
        try {
            onDevice = SpeechRecognizer.createOnDeviceSpeechRecognizer(this);
            Log.i(TAG, "createOnDeviceSpeechRecognizer: " + (onDevice != null));
        } catch (UnsupportedOperationException e) {
            Log.i(TAG, "createOnDeviceSpeechRecognizer: UnsupportedOperationException");
        }
        if (onDevice != null) onDevice.destroy();
    }

    private void listen() {
        SpeechRecognizer recognizer = SpeechRecognizer.createSpeechRecognizer(this);
        recognizer.setRecognitionListener(new RecognitionListener() {
            @Override
            public void onReadyForSpeech(Bundle params) {
                Log.i(TAG, "listener: onReadyForSpeech");
            }

            @Override
            public void onBeginningOfSpeech() {
                Log.i(TAG, "listener: onBeginningOfSpeech");
            }

            @Override
            public void onRmsChanged(float rmsdB) {
            }

            @Override
            public void onBufferReceived(byte[] buffer) {
            }

            @Override
            public void onEndOfSpeech() {
                Log.i(TAG, "listener: onEndOfSpeech");
            }

            @Override
            public void onError(int error) {
                Log.i(TAG, "listener: onError " + error);
                recognizer.destroy();
                mMain.postDelayed(SpeechApplication.this::finish, 1000);
            }

            @Override
            public void onResults(Bundle results) {
                Log.i(TAG, "listener: onResults");
            }

            @Override
            public void onPartialResults(Bundle partialResults) {
                Log.i(TAG, "listener: onPartialResults");
            }

            @Override
            public void onEvent(int eventType, Bundle params) {
            }
        });
        Intent intent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        intent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        Log.i(TAG, "startListening");
        recognizer.startListening(intent);
        Log.i(TAG, "startListening returned");
        mMain.postDelayed(this::finish, 15000);
    }
}
