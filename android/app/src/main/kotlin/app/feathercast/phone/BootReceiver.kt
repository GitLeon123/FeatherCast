package app.feathercast.phone

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Reconnects to the paired PC after a reboot or app update. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED || intent.action == Intent.ACTION_MY_PACKAGE_REPLACED) {
            if (PhoneApp.instance.store.load() != null) LinkService.start(context)
        }
    }
}
