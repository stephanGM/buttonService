package com.google.hal.buttonservice;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.util.Log;
import android.widget.Toast;


public class ButtonService extends Service {

    private static Context MyContext; /* get context to use from JNI */
    private static final String TAG = "PushbuttonService";

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    public void onDestroy() {
        Toast.makeText(this, "GPIO Interface Terminated", Toast.LENGTH_SHORT).show();
        Log.d(TAG, "onDestroy");
    }

    @Override
    public int onStartCommand(Intent intent,int flags, int startid)
    {
        MyContext = getApplicationContext(); /* get the context to use later from JNI */
        Toast.makeText(this, "GPIO Interface Running", Toast.LENGTH_SHORT).show();
        Log.d(TAG, "onStart");
        startRoutine(); /* call the C fn that begins the ISR thread w desired gpio pin #s */
        return START_STICKY;
    }

    /**
     * ====================================================================
     * showToast method:
     *   Used to display Toast messages using Boast and from worker threads
     * ====================================================================
     * Details:
     *   uses a handler in order to make Toast/Boast possible from worker
     *   threads
     * ====================================================================
     * authors(s): Stephan Greto-McGrath
     * ====================================================================
     */
    public void showToast(final String msg) {
        Handler hand = new Handler(MyContext.getMainLooper());
        hand.post(new Runnable() {
            @Override
            public void run() {
                Boast boast1 = Boast.makeText(MyContext, msg, Toast.LENGTH_SHORT);
                boast1.show();
            }
        });
    }
    // TODO edit this description
    /**
     * ====================================================================
     * handleStateChange method:
     *   will call showToast to display a string indication the direction
     *   of rotation of the rotary encoder. This method is called through
     *   JNI once the direction is determined by the ISR
     * ====================================================================
     * authors(s): Stephan Greto-McGrath
     * ====================================================================
     */
    public void handleStateChange(int direction){
        String direc;
        if (direction == 0){
            direc = "COUNTER_CLOCKWISE";
        }else if (direction == 1){
            direc = "CLOCKWISE";
        }else{
            direc = "invalid";
        }
//        showToast(direc);
        Log.d(TAG, direc);
        broadcastDirection(direc);

    }


    public void broadcastDirection(String direc){
        Intent i = new Intent();
        i.setAction("com.google.hal." + direc);
        MyContext.sendBroadcast(i);
    }

    /* load the library for JNI functionality */
    static {
        System.loadLibrary("button_service");
    }
    /* expose the C function to be called through JNI */
    public static native int startRoutine();
}