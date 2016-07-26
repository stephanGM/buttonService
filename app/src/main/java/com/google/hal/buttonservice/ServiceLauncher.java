package com.google.hal.buttonservice;

/**
 * ====================================================================
 * ServiceLaunce.java:
 *   Launcher to begin PushbuttonService.java
 *   in case it doesn't run on start on terminates for some reason
 * ====================================================================
 * authors(s): Stephan Greto-McGrath
 * ====================================================================
 */
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.content.Intent;
import android.widget.Toast;
import android.util.Log;
public class ServiceLauncher extends AppCompatActivity  {
    private static final String TAG = "ServiceLauncher";
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        Log.d(TAG,"manual start");
        Toast.makeText(getBaseContext(), "Service manually initiated", Toast.LENGTH_SHORT).show();
        super.onCreate(savedInstanceState);
        startService(new Intent(this, ButtonService.class));
        finish();
    }

}
