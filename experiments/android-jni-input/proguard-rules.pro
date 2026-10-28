# Static JNI names and the later explicit hook reference must survive any R8 pass.
-keep class org.mta.mobile.input.NativeInputBridge { *; }
-keep class org.mta.mobile.input.NativeInputBridge$Frame { *; }
-keep class org.mta.mobile.input.MotionEventInput { *; }
