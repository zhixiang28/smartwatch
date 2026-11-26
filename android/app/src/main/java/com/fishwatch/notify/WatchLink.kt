package com.fishwatch.notify

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.ArrayDeque
import java.util.UUID

/**
 * One background BLE link to the watch (same GATT service the phone settings page uses):
 * write the pairing code to AUTH (…0002), then each notification to NOTIF (…0009) as
 * "app\ntitle\ntext". autoConnect = true, so Android reconnects whenever the watch is back.
 */
@SuppressLint("MissingPermission")
object WatchLink {
    private const val TAG = "WatchLink"
    private fun uuid(n: Int) = UUID.fromString("f15a000$n-6c2b-4b9a-9d3e-2a1c5e7b8d00")
    private val SVC = uuid(1)
    private val AUTH = uuid(2)
    private val NOTIF = uuid(9)

    @Volatile var state = "未连接"
        private set
    private var gatt: BluetoothGatt? = null
    private var ready = false
    private var busy = false
    private val queue = ArrayDeque<ByteArray>()
    private val main = Handler(Looper.getMainLooper())
    private lateinit var app: Context

    fun prefs(ctx: Context) = ctx.getSharedPreferences("watch", Context.MODE_PRIVATE)

    fun connect(ctx: Context) { main.post {
        app = ctx.applicationContext
        val p = prefs(app)
        val mac = p.getString("mac", null) ?: run { state = "还没选手表"; return@post }
        if (gatt != null) return@post
        val adapter = (app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (adapter == null || !adapter.isEnabled) { state = "手机蓝牙没开"; return@post }
        state = "连接中…（手表要开着「收通知」）"
        gatt = try {
            adapter.getRemoteDevice(mac).connectGatt(app, true, cb, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) { state = "没有蓝牙权限"; null }
    } }

    /** forget the current link (new watch or new code) and connect again */
    fun reset(ctx: Context) { main.post {
        gatt?.close(); gatt = null; ready = false; busy = false; queue.clear()
        connect(ctx)
    } }

    fun send(appName: String, title: String, text: String) { main.post {
        var msg = "$appName\n$title\n$text".toByteArray(Charsets.UTF_8)
        if (msg.size > 500) msg = msg.copyOf(500).let { cutUtf8(it) }   // watch buffer is 600
        if (queue.size > 20) queue.poll()
        queue.add(msg)
        pump()
    } }

    private fun cutUtf8(b: ByteArray): ByteArray {      // don't end inside a multi-byte character
        var n = b.size
        while (n > 0 && (b[n - 1].toInt() and 0xC0) == 0x80) n--
        if (n > 0 && (b[n - 1].toInt() and 0x80) != 0) n--
        return b.copyOf(n)
    }

    private fun write(c: BluetoothGattCharacteristic, v: ByteArray): Boolean {
        val g = gatt ?: return false
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, v, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run { c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT; c.value = v; g.writeCharacteristic(c) }
        }
    }

    private fun pump() {
        if (!ready || busy) return
        val c = gatt?.getService(SVC)?.getCharacteristic(NOTIF) ?: return
        val next = queue.peek() ?: return
        busy = write(c, next)
        if (!busy) Log.w(TAG, "write refused")
    }

    private val cb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) { main.post {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                state = "已连上，准备中…"
                g.requestMtu(247)
            } else {
                ready = false; busy = false
                state = "等手表…（手表要开着「收通知」）"
                // autoConnect: Android reconnects by itself; keep the gatt object
            }
        } }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) { main.post { g.discoverServices() } }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) { main.post {
            val auth = g.getService(SVC)?.getCharacteristic(AUTH)
            if (auth == null) { state = "手表固件太旧（要 0.8.0 以上）"; return@post }
            val code = prefs(app).getString("code", "") ?: ""
            if (code.length != 4) { state = "还没填配对码"; return@post }
            if (!write(auth, code.toByteArray())) state = "送不出配对码"
        } }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) { main.post {
            if (c.uuid == AUTH) {
                if (status == BluetoothGatt.GATT_SUCCESS) { ready = true; state = "已连接手表" ; pump() }
                else state = "配对码不对（看手表：手机设定）"
            } else if (c.uuid == NOTIF) {
                busy = false
                if (status == BluetoothGatt.GATT_SUCCESS) queue.poll() else { ready = false; state = "手表拒收，重新配对中…"; g.discoverServices() }
                pump()
            }
        } }
    }
}
