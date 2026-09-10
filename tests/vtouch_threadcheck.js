var vt = require("/sdcard/vtouch_bundle.js");
try {
    var same = android.os.Looper.myLooper() === android.os.Looper.getMainLooper();
    log("THREAD name=" + java.lang.Thread.currentThread().getName() + " isMain=" + same);
    log("ROT=" + vt.rot() + " WH=" + device.width + "x" + device.height);
} catch (e) { log("THREAD-ERR " + e); }
exit();
