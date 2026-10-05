package app.feathercast.phone

enum class PhotoAccess { Full, Selected, Denied }

fun resolvePhotoAccess(sdk: Int, full: Boolean, selected: Boolean): PhotoAccess = when {
    full -> PhotoAccess.Full
    sdk >= 34 && selected -> PhotoAccess.Selected
    else -> PhotoAccess.Denied
}
