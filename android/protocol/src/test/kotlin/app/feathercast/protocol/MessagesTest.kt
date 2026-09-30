package app.feathercast.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

// Shared with TestMessageFixtures() in native/tests/phone_protocol_tests.cpp:
// the phone → PC strings must be exactly what these builders produce, and the
// PC → phone strings are exactly what the C++ builders produce.
class MessagesTest {
    @Test
    fun phoneToPcFixtures() {
        assertEquals(
            """{"type":"status","battery":76,"charging":true,"name":"Pixel","features":["media","sms"]}""",
            PhoneMessages.status(76, true, "Pixel", listOf("media", "sms")),
        )
        assertEquals(
            """{"type":"notification.posted","key":"k1","app":"com.whatsapp","appName":"WhatsApp","title":"Anna","text":"Hi","time":1000,"actions":[{"i":0,"title":"Reply","reply":true},{"i":1,"title":"Mark as read","reply":false}]}""",
            PhoneMessages.notificationPosted(
                "k1", "com.whatsapp", "WhatsApp", "Anna", "Hi", 1000,
                listOf(ActionInfo(0, "Reply", true), ActionInfo(1, "Mark as read", false)),
            ),
        )
        assertEquals(
            """{"type":"file.received","id":"ab12","name":"a.txt","ok":false,"error":"Storage full"}""",
            PhoneMessages.fileReceived("ab12", "a.txt", false, "Storage full"),
        )
        assertEquals("""{"type":"ring.state","ringing":true}""", PhoneMessages.ringState(true))
        assertEquals(
            """{"type":"media.state","app":"com.spotify.music","appName":"Spotify","title":"Song","artist":"Band","playing":true,"pos":1500,"dur":200000,"posAt":4000,"vol":7,"volMax":15}""",
            PhoneMessages.mediaState(MediaInfo("com.spotify.music", "Spotify", "Song", "Band", true, 1500, 200000, 4000, 7, 15)),
        )
        assertEquals("""{"type":"media.none"}""", PhoneMessages.mediaNone())
        assertEquals(
            """{"type":"sms.threads","items":[{"thread":"12","address":"+4917012345","name":"Anna","snippet":"See you","time":3000,"unread":true}]}""",
            PhoneMessages.smsThreads(listOf(SmsThreadInfo("12", "+4917012345", "Anna", "See you", 3000, true))),
        )
        assertEquals(
            """{"type":"sms.messages","thread":"12","items":[{"id":"7","body":"Hello","time":2000,"out":true}]}""",
            PhoneMessages.smsMessages("12", listOf(SmsMessageInfo("7", "Hello", 2000, true))),
        )
        assertEquals(
            """{"type":"sms.received","thread":"12","address":"+4917012345","name":"Anna","body":"Hi","time":6000}""",
            PhoneMessages.smsReceived("12", "+4917012345", "Anna", "Hi", 6000),
        )
        assertEquals("""{"type":"sms.sent","ref":"r1","ok":true}""", PhoneMessages.smsSent("r1", true))
        assertEquals(
            """{"type":"call.state","state":"ringing","number":"+4917012345","name":"Anna"}""",
            PhoneMessages.callState(CallState.Ringing, "+4917012345", "Anna"),
        )
        assertEquals(
            """{"type":"files.list","path":"/Download","items":[{"name":"Music","dir":true,"size":0,"time":100},{"name":"a.pdf","dir":false,"size":2048,"time":200}]}""",
            PhoneMessages.filesList("/Download", listOf(FileEntry("Music", true, 0, 100), FileEntry("a.pdf", false, 2048, 200))),
        )
        assertEquals(
            """{"type":"files.list","path":"/","error":"No access","items":[]}""",
            PhoneMessages.filesList("/", emptyList(), "No access"),
        )
        assertEquals(
            """{"type":"file.data","path":"/Download/a.pdf","name":"a.pdf"}""",
            PhoneMessages.fileData("/Download/a.pdf", "a.pdf"),
        )
    }

    @Test
    fun pcToPhoneFixtures() {
        assertEquals(
            PcMessage.NotificationAction("k1", 0, "On my way"),
            PcMessage.parse("""{"type":"notification.action","key":"k1","i":0,"text":"On my way"}"""),
        )
        assertEquals(
            PcMessage.NotificationAction("k1", 1, ""),
            PcMessage.parse("""{"type":"notification.action","key":"k1","i":1}"""),
        )
        assertEquals(PcMessage.FileSend("ab12", "a.txt"), PcMessage.parse("""{"type":"file.send","id":"ab12","name":"a.txt"}"""))
        assertEquals(PcMessage.Ring(true), PcMessage.parse("""{"type":"ring.start"}"""))
        assertEquals(PcMessage.Ring(false), PcMessage.parse("""{"type":"ring.stop"}"""))
        assertEquals(PcMessage.MediaCommand("toggle", -1), PcMessage.parse("""{"type":"media.command","cmd":"toggle"}"""))
        assertEquals(
            PcMessage.MediaCommand("seek", 30000),
            PcMessage.parse("""{"type":"media.command","cmd":"seek","pos":30000}"""),
        )
        assertEquals(PcMessage.MediaVolume(9), PcMessage.parse("""{"type":"media.volume","vol":9}"""))
        assertEquals(PcMessage.SmsThreadsRequest, PcMessage.parse("""{"type":"sms.threads.request"}"""))
        assertEquals(
            PcMessage.SmsMessagesRequest("12", 50),
            PcMessage.parse("""{"type":"sms.messages.request","thread":"12","limit":50}"""),
        )
        assertEquals(
            PcMessage.SmsSend("r1", "+4917012345", "Hi"),
            PcMessage.parse("""{"type":"sms.send","ref":"r1","address":"+4917012345","body":"Hi"}"""),
        )
        assertEquals(PcMessage.CallReject, PcMessage.parse("""{"type":"call.reject"}"""))
        assertEquals(PcMessage.CallSilence, PcMessage.parse("""{"type":"call.silence"}"""))
        assertEquals(
            PcMessage.FilesListRequest("/Download"),
            PcMessage.parse("""{"type":"files.list.request","path":"/Download"}"""),
        )
        assertEquals(
            PcMessage.FileRequest("/Download/a.pdf"),
            PcMessage.parse("""{"type":"file.request","path":"/Download/a.pdf"}"""),
        )
        assertNull(PcMessage.parse("""{"type":"future.thing"}"""))
        assertNull(PcMessage.parse("not json"))
    }

    @Test
    fun storagePaths() {
        assertEquals("/", normalizeStoragePath("/"))
        assertEquals("/", normalizeStoragePath(""))
        assertEquals("/Download/a.pdf", normalizeStoragePath("/Download//./a.pdf"))
        assertEquals("/DCIM/Camera", normalizeStoragePath("\\DCIM\\Camera\\"))
        assertNull(normalizeStoragePath("/Download/../../data"))
    }
}
