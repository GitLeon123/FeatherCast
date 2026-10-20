package app.feathercast.phone

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager

class PhoneApp : Application() {
    lateinit var store: LinkStore
        private set
    lateinit var link: LinkManager
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        store = LinkStore(this)
        link = LinkManager(this, store)
        val channel = NotificationChannel(
            LinkService.CHANNEL_ID,
            getString(R.string.channel_link),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = getString(R.string.channel_link_description)
            setShowBadge(false)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        IncomingFiles.createChannel(this)
        Ringer.createChannel(this)
        CallWatcher.restoreAfterRestart(this)
        if (store.load() != null) LinkService.start(this)
    }

    companion object {
        lateinit var instance: PhoneApp
            private set
    }
}
