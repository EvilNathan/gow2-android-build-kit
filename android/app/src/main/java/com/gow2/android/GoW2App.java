package com.gow2.android;

import android.app.Application;

/** Prepares the shared log file before any activity or native code starts. */
public final class GoW2App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        LogFile.prepare(this);
        LogFile.installHandler(this);
    }
}
