# Consumer ProGuard / R8 rules for dev.weft:weft-core
# Keep native JNI methods in dev.weft.**
-keepclasseswithmembernames class dev.weft.** {
    native <methods>;
}

# Keep the TriadNative object and its JNI entry points
-keep class dev.weft.TriadNative {
    *;
}

# Keep atomic fields on Weft and Steward
-keepclassmembers class dev.weft.Weft {
    private volatile int wWork;
    private volatile int rWork;
}
