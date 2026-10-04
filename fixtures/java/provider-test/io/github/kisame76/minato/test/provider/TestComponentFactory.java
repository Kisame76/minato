package io.github.kisame76.minato.test.provider;

import android.app.Activity;
import android.app.AppComponentFactory;
import android.app.Application;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.Intent;
import android.util.Log;

/**
 * ADR-0039 step c2c1's AppComponentFactory: it overrides the five instantiate methods, as androidx's
 * CoreComponentFactory does, so that ART resolves each one's types against the framework's as it initializes the
 * class, and logs each call. The activity's goes under the device tag: Android starts the launcher activity after
 * the Application's onCreate, and Minato starts none before step c2c3.
 */
public class TestComponentFactory extends AppComponentFactory {
    @Override
    public Application instantiateApplication(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Log.i(ProviderApplication.TAG, "factory instantiateApplication " + className);
        return super.instantiateApplication(cl, className);
    }

    @Override
    public Activity instantiateActivity(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Log.i(ProviderApplication.TAG + "-device", "factory instantiateActivity " + className);
        return super.instantiateActivity(cl, className, intent);
    }

    @Override
    public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Log.i(ProviderApplication.TAG, "factory instantiateReceiver " + className);
        return super.instantiateReceiver(cl, className, intent);
    }

    @Override
    public Service instantiateService(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Log.i(ProviderApplication.TAG, "factory instantiateService " + className);
        return super.instantiateService(cl, className, intent);
    }

    @Override
    public ContentProvider instantiateProvider(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Log.i(ProviderApplication.TAG, "factory instantiateProvider " + className);
        return super.instantiateProvider(cl, className);
    }
}
