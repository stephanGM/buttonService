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
    private static final String TAG = "buttonService";

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    public void onDestroy() {
        Toast.makeText(this, "Pushbutton Interface Terminated", Toast.LENGTH_SHORT).show();
        Log.d(TAG, "onDestroy");
    }

    @Override
    public int onStartCommand(Intent intent,int flags, int startid)
    {
        MyContext = getApplicationContext(); /* get the context to use later from JNI */
        Toast.makeText(this, "Pushbutton Interface Running", Toast.LENGTH_SHORT).show();
        Log.d(TAG, "onStart");
        startRoutine(); /* call the C fn that begins the ISR thread*/
        return START_STICKY;
    }

    /**
     * ====================================================================
     * showToast method:
     *   Used to display Toast messages using Boast from worker threads
     *   without jamming up the UI thread.
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
     * jniReturn method:
     *   Receives the type of action (short, long or double press) as well
     *   as the button the input occured on. Based on this it formats a
     *   string "action" and calls broadcastAction with it.
     * ====================================================================
     * authors(s): Stephan Greto-McGrath
     * ====================================================================
     */
    public void jniReturn(int action, int button){
        String broadcast_action;
        switch(action){
            case 0:
                broadcast_action = "SHORT_" + Integer.toString(button);
                break;
            case 1:
                broadcast_action = "LONG_" + Integer.toString(button);
                break;
            case 2:
                broadcast_action = "DOUBLE_" + Integer.toString(button);
                break;
            case 3:
                broadcast_action = "BUTTON_DOWN_" + Integer.toString(button);
                break;
            case 4:
                broadcast_action = "BUTTON_UP_" + Integer.toString(button);
                break;
            default:
                broadcast_action = "INVALID";
                break;
        }
//        showToast(broadcast_action);
        Log.d(TAG, broadcast_action);
        if (!broadcast_action.equals("INVALID")){
            broadcastAction(broadcast_action);
        }


    }

    /**
     * ====================================================================
     * broadcastAction method:
     *   Pretty self explanatory, it broadcasts the type of input action
     *   with the button the input occured on for other apps to pick up and
     *   act on.
     * ====================================================================
     * authors(s): Stephan Greto-McGrath
     * ====================================================================
     */
    public void broadcastAction(String action){
        Intent i = new Intent();
        i.setAction("com.google.hal." + action);
        MyContext.sendBroadcast(i);
    }

    /* load the library for JNI functionality */
    static {
        System.loadLibrary("button_service");
    }
    /* expose the C function to be called through JNI */
    public static native int startRoutine();
}
