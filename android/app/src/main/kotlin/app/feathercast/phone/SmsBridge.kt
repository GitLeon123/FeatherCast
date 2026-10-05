package app.feathercast.phone

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.ContactsContract
import android.provider.Telephony
import android.telephony.SmsManager
import app.feathercast.protocol.PhoneMessages
import app.feathercast.protocol.SmsMessageInfo
import app.feathercast.protocol.SmsThreadInfo
import java.util.concurrent.ConcurrentHashMap

/** Reads and sends text messages (SMS only, no MMS) for the PC. */
object SmsBridge {
    val permissions = arrayOf(
        Manifest.permission.READ_SMS,
        Manifest.permission.SEND_SMS,
        Manifest.permission.RECEIVE_SMS,
        Manifest.permission.READ_CONTACTS,
    )

    private val names = ConcurrentHashMap<String, String>()

    fun hasPermission(context: Context): Boolean =
        permissions.take(3).all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun canReadContacts(context: Context) =
        context.checkSelfPermission(Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED

    val active: Boolean
        get() = PhoneApp.instance.store.smsAccess && hasPermission(PhoneApp.instance)

    /** Contact name for a phone number, or "" when unknown. */
    fun contactName(context: Context, address: String): String {
        if (address.isEmpty() || !canReadContacts(context)) return ""
        return names.getOrPut(address) {
            try {
                val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(address))
                context.contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)
                    ?.use { if (it.moveToFirst()) it.getString(0).orEmpty() else "" } ?: ""
            } catch (_: Exception) {
                ""
            }
        }
    }

    /** Newest conversations, one entry per thread. */
    fun threads(context: Context, limit: Int = 100): List<SmsThreadInfo> {
        val result = LinkedHashMap<String, SmsThreadInfo>()
        val unread = HashSet<String>()
        try {
            context.contentResolver.query(
                Telephony.Sms.CONTENT_URI,
                arrayOf(Telephony.Sms.THREAD_ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.READ, Telephony.Sms.TYPE),
                null, null, "${Telephony.Sms.DATE} DESC",
            )?.use { cursor ->
                var scanned = 0
                while (cursor.moveToNext() && scanned++ < 5000) {
                    val thread = cursor.getLong(0).toString()
                    if (cursor.getInt(4) == 0 && cursor.getInt(5) == Telephony.Sms.MESSAGE_TYPE_INBOX) unread.add(thread)
                    if (thread in result) continue
                    if (result.size >= limit) continue
                    val address = cursor.getString(1).orEmpty()
                    result[thread] = SmsThreadInfo(
                        thread = thread,
                        address = address,
                        name = contactName(context, address),
                        snippet = cursor.getString(2).orEmpty().take(200),
                        time = cursor.getLong(3),
                        unread = false,
                    )
                }
            }
        } catch (_: SecurityException) {
            return emptyList()
        }
        return result.values.map { it.copy(unread = it.thread in unread) }
    }

    /** The newest messages of one conversation, oldest first. */
    fun messages(context: Context, thread: String, limit: Int): List<SmsMessageInfo> {
        val id = thread.toLongOrNull() ?: return emptyList()
        val out = ArrayList<SmsMessageInfo>()
        try {
            context.contentResolver.query(
                Telephony.Sms.CONTENT_URI,
                arrayOf(Telephony.Sms._ID, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.TYPE),
                "${Telephony.Sms.THREAD_ID} = ?", arrayOf(id.toString()), "${Telephony.Sms.DATE} DESC",
            )?.use { cursor ->
                while (cursor.moveToNext() && out.size < limit.coerceIn(1, 200)) {
                    out.add(
                        SmsMessageInfo(
                            id = cursor.getLong(0).toString(),
                            body = cursor.getString(1).orEmpty(),
                            time = cursor.getLong(2),
                            outgoing = cursor.getInt(3) != Telephony.Sms.MESSAGE_TYPE_INBOX,
                        ),
                    )
                }
            }
        } catch (_: SecurityException) {
        }
        return out.reversed()
    }

    fun threadId(context: Context, address: String): String =
        try {
            Telephony.Threads.getOrCreateThreadId(context, address).toString()
        } catch (_: Exception) {
            ""
        }

    fun send(context: Context, ref: String, address: String, body: String) {
        val link = LinkManager.instance
        if (!active || address.isBlank() || body.isEmpty()) {
            link.sendAsync(PhoneMessages.smsSent(ref, false, "Text messages are turned off on the phone."))
            return
        }
        val manager = if (Build.VERSION.SDK_INT >= 31) {
            context.getSystemService(SmsManager::class.java)
        } else {
            @Suppress("DEPRECATION")
            SmsManager.getDefault()
        }
        try {
            val parts = manager.divideMessage(body)
            val count = parts.size.coerceAtLeast(1)
            val sent = ArrayList(List(count) { part -> SmsSentReceiver.pendingIntent(context, ref, part, count) })
            if (parts.size <= 1) {
                manager.sendTextMessage(address, null, body, sent[0], null)
            } else {
                manager.sendMultipartTextMessage(address, null, parts, sent, null)
            }
        } catch (error: Exception) {
            link.sendAsync(PhoneMessages.smsSent(ref, false, error.message ?: "Could not send the message."))
        }
    }
}

/** Reports whether an SMS sent for the PC went out, once every part has a result. */
class SmsSentReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val ref = intent.getStringExtra(EXTRA_REF) ?: return
        val parts = intent.getIntExtra(EXTRA_PARTS, 1).coerceAtLeast(1)
        val ok = resultCode == Activity.RESULT_OK
        val progress = pending.merge(ref, Progress(1, ok)) { a, b -> Progress(a.done + b.done, a.ok && b.ok) } ?: return
        if (progress.done < parts || !pending.remove(ref, progress)) return
        LinkManager.instance.sendAsync(
            PhoneMessages.smsSent(ref, progress.ok, if (progress.ok) "" else "The phone could not send the message."),
        )
    }

    private data class Progress(val done: Int, val ok: Boolean)

    companion object {
        private const val EXTRA_REF = "ref"
        private const val EXTRA_PARTS = "parts"
        private val pending = ConcurrentHashMap<String, Progress>()

        /** One distinct PendingIntent per part (the data Uri), so none replaces another. */
        fun pendingIntent(context: Context, ref: String, part: Int, parts: Int): PendingIntent = PendingIntent.getBroadcast(
            context, 0,
            Intent(context, SmsSentReceiver::class.java)
                .setData(Uri.Builder().scheme("feathercast-sms").authority("sent").appendPath(ref).appendPath(part.toString()).build())
                .putExtra(EXTRA_REF, ref).putExtra(EXTRA_PARTS, parts),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
    }
}

/** Forwards incoming text messages to the PC's Messages view. */
class SmsReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Telephony.Sms.Intents.SMS_RECEIVED_ACTION) return
        if (!SmsBridge.active || !LinkManager.instance.isConnected) return
        val messages = Telephony.Sms.Intents.getMessagesFromIntent(intent) ?: return
        if (messages.isEmpty()) return
        val address = messages[0].originatingAddress.orEmpty()
        val body = messages.joinToString("") { it.messageBody.orEmpty() }
        val time = messages[0].timestampMillis.takeIf { it > 0 } ?: System.currentTimeMillis()
        val pending = goAsync()
        Thread {
            try {
                val app = context.applicationContext
                LinkManager.instance.send(
                    PhoneMessages.smsReceived(SmsBridge.threadId(app, address), address, SmsBridge.contactName(app, address), body, time),
                )
            } finally {
                pending.finish()
            }
        }.start()
    }
}
