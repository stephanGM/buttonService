# Push Button Service
A service for Android

## Overview

The Push Button Service is a simple, portable, service APK for Android that serves as an interface between a push button and the android device. The service detects logic level changes on GPIO pins through the sysfs interface. Using this information, it is able to determine whether a short, long or double press has occurred. It then broadcasts an intent which can be picked up and used by other applications in whatever way they decide to act on this input.
