package com.fishwatch.notify

import android.app.Notification
import android.content.pm.PackageManager
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

/** Forwards each new notification to the watch. Android keeps this service bound (and the
 *  process alive) while notification access is granted, so the BLE link lives here. */
class NotifListener : NotificationListenerService() {
    private var lastKey = ""
    private var lastAt = 0L

    override fun onListenerConnected() { WatchLink.connect(this) }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val n = sbn.notification ?: return
        if (sbn.packageName == packageName) return
        if (sbn.isOngoing) return                                         // music, navigation, downloads…
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return       // "3 new messages" summaries
        if (WatchLink.prefs(this).getStringSet("muted", emptySet())!!.contains(sbn.packageName)) return
        val ex = n.extras
        val title = ex.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val text = (ex.getCharSequence(Notification.EXTRA_BIG_TEXT) ?: ex.getCharSequence(Notification.EXTRA_TEXT))?.toString() ?: ""
        if (title.isEmpty() && text.isEmpty()) return
        val key = sbn.packageName + title + text
        val now = System.currentTimeMillis()
        if (key == lastKey && now - lastAt < 3000) return                   // apps re-post the same one
        lastKey = key; lastAt = now
        val label = try {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString()
        } catch (e: PackageManager.NameNotFoundException) { sbn.packageName }
        WatchLink.send(label, title, text)
    }
}
