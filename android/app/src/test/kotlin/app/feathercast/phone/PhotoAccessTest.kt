package app.feathercast.phone

import org.junit.Assert.assertEquals
import org.junit.Test

class PhotoAccessTest {
    @Test fun preservesLanAccessBeforeTarget37() {
        assertEquals(false, needsLocalNetworkPermission(37, 35))
        assertEquals(false, needsLocalNetworkPermission(35, 37))
        assertEquals(true, needsLocalNetworkPermission(37, 37))
    }
    @Test fun resolvesFullSelectedAndDenied() {
        assertEquals(PhotoAccess.Full, resolvePhotoAccess(34, true, true))
        assertEquals(PhotoAccess.Selected, resolvePhotoAccess(34, false, true))
        assertEquals(PhotoAccess.Denied, resolvePhotoAccess(34, false, false))
        assertEquals(PhotoAccess.Denied, resolvePhotoAccess(33, false, true))
        assertEquals(PhotoAccess.Full, resolvePhotoAccess(26, true, false))
    }
}
