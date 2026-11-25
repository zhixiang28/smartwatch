package com.fishwatch.notify

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Setup screen, built in code (no layout files): code, pick the watch, permissions, test. */
@SuppressLint("MissingPermission", "SetTextI18n")
class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var watchTv: TextView
    private lateinit var code: EditText
    private val ui = Handler(Looper.getMainLooper())
    private var scanning = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val p = WatchLink.prefs(this)
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(48, 64, 48, 48) }
        fun text(s: String, size: Float = 16f) = TextView(this).apply { this.text = s; textSize = size; setPadding(0, 18, 0, 6) }
        fun button(s: String, f: () -> Unit) = Button(this).apply { text = s; setOnClickListener { f() } }

        col.addView(text("FishWatch 通知", 24f))
        col.addView(text("把手机通知转到手表。手表要刷 0.8.0 以上，并在手表「通知」里打开「收通知」。"))
        status = text("", 18f); col.addView(status)

        col.addView(text("1. 配对码（手表：应用 → 手机设定 → 开启蓝牙配对，看到的 4 位数）"))
        code = EditText(this).apply { inputType = InputType.TYPE_CLASS_NUMBER; setText(p.getString("code", "")); hint = "4 位数" }
        col.addView(code)
        col.addView(button("存配对码") {
            p.edit().putString("code", code.text.toString().trim()).apply(); WatchLink.reset(this)
        })

        col.addView(text("2. 选手表"))
        watchTv = text(p.getString("name", null)?.let { "已选：$it" } ?: "还没选"); col.addView(watchTv)
        col.addView(button("搜寻手表") { scan() })

        col.addView(text("3. 允许读取通知"))
        col.addView(button("打开「通知使用权」设定") {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        })
        col.addView(button("不要限制电池（背景才不会被关掉）") {
            val pm = getSystemService(PowerManager::class.java)
            if (!pm.isIgnoringBatteryOptimizations(packageName))
                startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        })

        col.addView(text("4. 试试看"))
        col.addView(button("送一条测试通知到手表") { WatchLink.send("FishWatch", "测试通知", "手表振动并显示这条，就设定好了。") })

        setContentView(ScrollView(this).apply { addView(col) })
        askPermissions()
    }

    override fun onResume() {
        super.onResume()
        WatchLink.connect(this)
        tick()
    }

    override fun onPause() { super.onPause(); ui.removeCallbacksAndMessages(null) }

    private fun tick() {
        val access = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
            ?.contains(ComponentName(this, NotifListener::class.java).flattenToString()) == true
        status.text = "状态：${WatchLink.state}" + if (access) "" else "\n还没允许读取通知（第 3 步）"
        ui.postDelayed({ tick() }, 1000)
    }

    private fun askPermissions() {
        val need = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = need.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), 1)
    }

    private fun scan() {
        askPermissions()
        val scanner = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter?.bluetoothLeScanner
            ?: run { watchTv.text = "手机蓝牙没开"; return }
        if (scanning) return
        scanning = true
        watchTv.text = "搜寻中…（手表要开着蓝牙）"
        val cb = object : ScanCallback() {
            override fun onScanResult(type: Int, r: ScanResult) {
                val name = r.scanRecord?.deviceName ?: r.device.name ?: return
                if (!name.startsWith("FishWatch")) return
                scanner.stopScan(this); scanning = false
                WatchLink.prefs(this@MainActivity).edit().putString("mac", r.device.address).putString("name", "$name ${r.device.address}").apply()
                watchTv.text = "已选：$name ${r.device.address}"
                WatchLink.reset(this@MainActivity)
            }
        }
        try { scanner.startScan(cb) } catch (e: SecurityException) { watchTv.text = "没有蓝牙权限"; scanning = false; return }
        ui.postDelayed({ if (scanning) { scanner.stopScan(cb); scanning = false; watchTv.text = "没找到。手表：应用 → 手机设定 → 开启蓝牙配对" } }, 12000)
    }
}
