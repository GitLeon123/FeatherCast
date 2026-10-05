package app.feathercast.phone

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build

fun needsLocalNetworkPermission(sdk: Int, targetSdk: Int): Boolean = sdk >= 37 && targetSdk >= 37

object LocalNetworkAccess {
    const val PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"
    fun required(context: Context): Boolean = needsLocalNetworkPermission(Build.VERSION.SDK_INT, context.applicationInfo.targetSdkVersion)
    fun allowed(context: Context): Boolean = !required(context) ||
        context.checkSelfPermission(PERMISSION) == PackageManager.PERMISSION_GRANTED
}
