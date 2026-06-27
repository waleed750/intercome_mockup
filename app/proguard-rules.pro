# Keep kotlinx.serialization generated serializers for our protocol models.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**

-keepclassmembers class com.smarthome.intercom.** {
    *** Companion;
}
-keepclasseswithmembers class com.smarthome.intercom.** {
    kotlinx.serialization.KSerializer serializer(...);
}
