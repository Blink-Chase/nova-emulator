package com.blinkchase.nova

import android.Manifest
import android.util.Log
import com.blinkchase.nova.ui.theme.NovaEmuTheme
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.view.Surface
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.LocalContext
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicLong

class MainActivity : ComponentActivity() {

    companion object {
        init {
            // Load libc++_shared first (bundled in jniLibs)
            try { System.loadLibrary("c++_shared") } catch (e: Exception) {}
            // Then load our native library
            System.loadLibrary("nova_native")
        }
        private const val PERMISSION_REQUEST_CODE = 1001
        const val PREFS_NAME = "nova_prefs"
        const val KEY_FAVORITES = "favorite_games"
        const val KEY_RECENTS = "recent_games"
        const val KEY_SCAN_MODE = "scan_mode"
        const val KEY_CUSTOM_PATHS = "custom_paths"
        const val KEY_AUDIO_LATENCY = "audio_latency"
        const val KEY_SHOW_FF = "show_ff"
        const val KEY_AUTO_PAUSE_MENU = "auto_pause_menu"
        const val KEY_CONTROLLER_STYLE = "controller_style"
        const val KEY_THEME_MODE = "theme_mode"
        const val BTN_B = 0; const val BTN_Y = 1; const val BTN_SELECT = 2; const val BTN_START = 3
        const val BTN_UP = 4; const val BTN_DOWN = 5; const val BTN_LEFT = 6; const val BTN_RIGHT = 7
        const val BTN_A = 8; const val BTN_X = 9; const val BTN_L = 10; const val BTN_R = 11
        const val BTN_L2 = 12; const val BTN_R2 = 13; const val BTN_L3 = 14; const val BTN_R3 = 15
        const val BTN_Z = 12
        const val KEY_LAST_CRASHED_CORE = "last_crashed_core"
        const val KEY_LAST_CRASHED_LAYOUT = "last_crashed_layout"

        val AVAILABLE_CORES = mapOf(
            Platform.SNES to listOf("snes9x_libretro_android", "snes9x2010_libretro_android"),
            Platform.GBA to listOf("mgba_libretro_android", "vba_next_libretro_android"),
            Platform.GB to listOf("mgba_libretro_android", "gambatte_libretro_android"),
            Platform.GBC to listOf("mgba_libretro_android", "gambatte_libretro_android"),
            Platform.GENESIS to listOf("genesis_plus_gx_libretro_android", "picodrive_libretro_android"),
            Platform.N64 to listOf("mupen64plus_next_libretro_android", "parallel_n64_libretro_android"),
            Platform.PS1 to listOf("pcsx_rearmed_libretro_android", "swanstation_libretro_android"),
            Platform.GAMECUBE to listOf("dolphin_libretro_android"),
            Platform.WII to listOf("dolphin_libretro_android")
        )
    }

    private var audioTrack: android.media.AudioTrack? = null
    private val audioLock = java.lang.Object()
    var targetSampleRate = 44100
    val samplesWritten = AtomicLong(0)
    
    // Advanced audio tracking for underrun detection
    private var audioErrorCount = 0
    private var lastAudioLogTime = 0L
    private var audioSamplesTotal = 0L
    private var audioUnderruns = 0
    private var audioInitCount = 0
    private var audioCutouts = 0
    private var lastPositionFrames = 0L
    private var lastPositionTime = 0L
    private var wasPlaying = false
    
    external fun loadCore(corePath: String): String?
    external fun nativeLoadGame(romPath: String): Boolean
    external fun nativePauseGame()
    external fun nativeResumeGame()
    external fun resetGame()
    external fun nativeQuitGame()
    external fun setSurface(surface: Surface?)
    external fun sendInput(buttonId: Int, value: Int)
    external fun updateNativeActivity()
    external fun setSystemDirectories(systemDir: String, saveDir: String)
    external fun saveState(path: String): Boolean
    external fun loadState(path: String): Boolean
    external fun setFastForward(enabled: Boolean)
    external fun setCheat(index: Int, enabled: Boolean, code: String)

    fun loadGame(romPath: String): Boolean { resetAudio(); return nativeLoadGame(romPath) }

    fun writeAudio(data: ShortArray, size: Int) {
        val currentTime = System.currentTimeMillis()
        
        // Check for audio cutouts by monitoring playback position
        val track = synchronized(audioLock) { audioTrack }
        
        if (track != null && track.playState == android.media.AudioTrack.PLAYSTATE_PLAYING) {
            if (!wasPlaying) {
                // Audio started playing
                wasPlaying = true
                lastPositionTime = currentTime
                lastPositionFrames = 0
                Log.i("Nova", "AUDIO: Playback started")
            }
            
            // Check actual playback position
            try {
                val currentPosition = track.playbackHeadPosition.toLong()
                
                if (lastPositionFrames > 0L && currentPosition < lastPositionFrames - 1000L) {
                    // Audio jumped backwards = CUTOUT!
                    audioCutouts++
                    val missedFrames = (lastPositionFrames - currentPosition)
                    Log.w("Nova", "AUDIO: CUTOUT #$audioCutouts! Missed ~$missedFrames frames")
                }
                
                lastPositionFrames = currentPosition
                lastPositionTime = currentTime
            } catch (e: Exception) {
                // Ignore position read errors
            }
        } else {
            wasPlaying = false
        }
        
        // Log audio stats every 5 seconds
        if (currentTime - lastAudioLogTime > 5000 && audioInitCount > 0) {
            val bufferMultiplier = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).getInt(KEY_AUDIO_LATENCY, 1)
            Log.i("Nova", "AUDIO: Stats - Total: ${audioSamplesTotal}, Cutouts: $audioCutouts, Errors: $audioErrorCount, LatMode: $bufferMultiplier")
            lastAudioLogTime = currentTime
        }
        
        val audioTrackLocal = synchronized(audioLock) {
            if (audioTrack == null) {
                try {
                    audioInitCount++
                    val latencyMode = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).getInt(KEY_AUDIO_LATENCY, 1)
                    val minSize = android.media.AudioTrack.getMinBufferSize(targetSampleRate, android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT)
                    if (minSize <= 0) {
                        Log.e("Nova", "AUDIO: ERROR - Invalid min buffer size: $minSize")
                        return
                    }
                    val bufferMultiplier = when(latencyMode) { 0 -> 3; 1 -> 6; 2 -> 10; else -> 6 }
                    val bufferSize = minSize * bufferMultiplier
                    Log.d("Nova", "AUDIO: Init - Rate: $targetSampleRate, MinBuf: $minSize, Mult: $bufferMultiplier, Size: $bufferSize")
                    audioTrack = android.media.AudioTrack.Builder()
                        .setAudioAttributes(android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_GAME).setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build())
                        .setAudioFormat(android.media.AudioFormat.Builder().setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT).setSampleRate(targetSampleRate).setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build())
                        .setBufferSizeInBytes(bufferSize).setTransferMode(android.media.AudioTrack.MODE_STREAM).build()
                    audioTrack?.play()
                    Log.i("Nova", "AUDIO: Track created")
                } catch (e: Exception) {
                    audioErrorCount++
                    Log.e("Nova", "AUDIO: ERROR init - ${e.message}")
                    return
                }
            }
            audioTrack
        }
        
        if (audioTrackLocal != null) {
            try {
                val result = audioTrackLocal.write(data, 0, size)
                if (result > 0) {
                    samplesWritten.addAndGet(result.toLong())
                    audioSamplesTotal += result
                } else if (result < 0) {
                    audioErrorCount++
                    audioCutouts++
                    Log.e("Nova", "AUDIO: WRITE FAIL #$audioCutouts code=$result")
                    resetAudio()
                }
            } catch (e: Exception) {
                audioErrorCount++
                Log.e("Nova", "AUDIO: Exception - ${e.message}")
            }
        }
    }

    fun resetAudio() { 
        synchronized(audioLock) { 
            try { 
                audioTrack?.release() 
                Log.d("Nova", "AUDIO: Track released")
            } catch (e: Exception) {} 
        }
        audioTrack = null 
        wasPlaying = false
        lastPositionFrames = 0
    }
    
    fun pauseGame() { 
        synchronized(audioLock) { 
            try { audioTrack?.pause() } catch (e: Exception) {} 
        }
        wasPlaying = false
        nativePauseGame() 
    }
    
    fun resumeGame() { 
        nativeResumeGame()
        synchronized(audioLock) { 
            try { audioTrack?.play() } catch (e: Exception) { 
                resetAudio()
                try { audioTrack?.play() } catch (e2: Exception) {} 
            } 
        } 
    }
    
    fun quitGame() { 
        synchronized(audioLock) { 
            try { audioTrack?.pause() } catch (e: Exception) {} 
        }
        wasPlaying = false
        nativeQuitGame(); 
        resetAudio() 
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                intent.data = Uri.parse("package:$packageName")
                startActivity(intent)
            }
        } else {
            val hasPermission = ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
            if (!hasPermission) {
                ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE, Manifest.permission.READ_EXTERNAL_STORAGE), PERMISSION_REQUEST_CODE)
            }
        }
        updateNativeActivity()
        setContent { 
            var themeMode by remember { mutableStateOf(0) }
            val prefs = remember { getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE) }
            LaunchedEffect(Unit) {
                themeMode = prefs.getInt(KEY_THEME_MODE, 0)
            }
            DisposableEffect(prefs) {
                val listener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
                    if (key == KEY_THEME_MODE) {
                        themeMode = prefs.getInt(KEY_THEME_MODE, 0)
                    }
                }
                prefs.registerOnSharedPreferenceChangeListener(listener)
                onDispose { prefs.unregisterOnSharedPreferenceChangeListener(listener) }
            }
            val isDarkTheme = when (themeMode) {
                1 -> false
                2 -> true
                else -> null
            }
            NovaEmuTheme(darkTheme = isDarkTheme) { 
                Surface(modifier = Modifier.fillMaxSize()) { NovaApp() } 
            } 
        }
    }

    override fun onStop() { super.onStop(); resetAudio() }
    override fun onDestroy() { super.onDestroy(); resetAudio() }

    @Composable
    fun NovaApp() {
        val context = LocalContext.current
        val prefs = remember { context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE) }
        val scope = rememberCoroutineScope()

        var gameList by remember { mutableStateOf(listOf<GameFile>()) }
        var currentScreen by rememberSaveable { mutableStateOf(Screen.HOME) }
        var activeGamePath by rememberSaveable { mutableStateOf("") }
        var activeGameName by rememberSaveable { mutableStateOf("") }
        var activeGamePlatform by rememberSaveable { mutableStateOf(Platform.UNKNOWN) }
        var activeGameCorePath by rememberSaveable { mutableStateOf("") }
        var buttonOffsets by remember { mutableStateOf(mapOf<Int, Offset>()) }
        var favoritePaths by remember { mutableStateOf(setOf<String>()) }
        var recentPaths by remember { mutableStateOf(listOf<String>()) }
        
        val storageDir = remember {
            val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
            val publicDir = File(documentsDir, "Nova")
            if (publicDir.exists() || publicDir.mkdirs()) publicDir else context.getExternalFilesDir(null) ?: context.filesDir
        }
        
        val savesDir = remember { File(storageDir, "saves").also { it.mkdirs() } }
        val layoutsDir = remember { File(storageDir, "layouts").also { it.mkdirs() } }
        val internalCoresDir = remember { File(context.filesDir, "cores").also { it.mkdirs() } }
        val coresDir = remember { File(storageDir, "cores").also { it.mkdirs() } }
        val systemDir = remember { File(storageDir, "system").also { it.mkdirs() } }
        
        LaunchedEffect(Unit) { setSystemDirectories(systemDir.absolutePath, savesDir.absolutePath) }

        fun scanCores(): List<String> = internalCoresDir.listFiles()?.filter { it.name.endsWith(".so") }?.map { it.name.removePrefix("lib").removeSuffix(".so") } ?: emptyList()
        fun scanLayouts(): List<String> = layoutsDir.listFiles()?.filter { it.extension == "layout" }?.map { it.nameWithoutExtension } ?: emptyList()

        fun performScan() {
            scope.launch(Dispatchers.IO) {
                Log.d("Nova", "SCAN: Starting ROM scan...")
                val mode = prefs.getInt(KEY_SCAN_MODE, 0)
                val pathsToScan = if (mode == 1) {
                    prefs.getStringSet(KEY_CUSTOM_PATHS, emptySet())?.map { File(it) } ?: emptyList()
                } else {
                    listOf(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS))
                }
                Log.d("Nova", "SCAN: Scanning ${pathsToScan.size} paths")
                val allGames = mutableListOf<GameFile>()
                pathsToScan.forEach { dir ->
                    Log.d("Nova", "SCAN: Checking directory: ${dir.absolutePath}")
                    if (dir.exists() && dir.isDirectory) { 
                        val found = RomScanner.scan(dir)
                        Log.d("Nova", "SCAN: Found ${found.size} games in ${dir.name}")
                        allGames.addAll(found)
                    } else {
                        Log.w("Nova", "SCAN: Directory does not exist: ${dir.absolutePath}")
                    }
                }
                Log.d("Nova", "SCAN: Total games found: ${allGames.size}")
                withContext(Dispatchers.Main) { gameList = allGames }
            }
        }

        LaunchedEffect(Unit) {
            var copiedCount = 0
            coresDir.listFiles()?.filter { it.name.endsWith(".so") }?.forEach { soFile ->
                val targetFile = File(internalCoresDir, soFile.name)
                if (!targetFile.exists() || targetFile.length() != soFile.length()) {
                    try {
                        soFile.copyTo(targetFile, overwrite = true)
                        copiedCount++
                    } catch (e: Exception) {}
                }
            }
            
            val installedCores = scanCores()
            val installedLayouts = scanLayouts()
            withContext(Dispatchers.Main) {
                android.widget.Toast.makeText(context, "Found ${installedCores.size} cores ($copiedCount imported), ${installedLayouts.size} layouts", android.widget.Toast.LENGTH_LONG).show()
            }
            performScan()
            favoritePaths = prefs.getStringSet(KEY_FAVORITES, emptySet()) ?: emptySet()
            val savedRecents = prefs.getString(KEY_RECENTS, "") ?: ""
            if (savedRecents.isNotEmpty()) { recentPaths = savedRecents.split("|") }
        }
        
        fun toggleFavorite(gamePath: String) {
            val newFavorites = favoritePaths.toMutableSet()
            if (newFavorites.contains(gamePath)) newFavorites.remove(gamePath) else newFavorites.add(gamePath)
            favoritePaths = newFavorites
            prefs.edit().putStringSet(KEY_FAVORITES, newFavorites).apply()
        }

        fun addToRecents(game: GameFile) {
            val newPaths = (listOf(game.path) + recentPaths.filter { it != game.path }).take(10)
            recentPaths = newPaths
            prefs.edit().putString(KEY_RECENTS, newPaths.joinToString("|")).apply()
        }

        fun saveLayout(gameName: String) {
            val layoutFile = File(layoutsDir, "$gameName.layout")
            val content = buttonOffsets.map { "${it.key},${it.value.x},${it.value.y}" }.joinToString("\n")
            try { layoutFile.writeText(content) } catch (e: Exception) {}
        }

        fun loadLayout(gameName: String): Map<Int, Offset> {
            val layoutFile = File(layoutsDir, "$gameName.layout")
            if (!layoutFile.exists()) return emptyMap()
            return try {
                layoutFile.readLines().mapNotNull { line ->
                    val parts = line.split(",")
                    if (parts.size == 3) {
                        val btnId = parts[0].toIntOrNull() ?: return@mapNotNull null
                        val x = parts[1].toFloatOrNull() ?: return@mapNotNull null
                        val y = parts[2].toFloatOrNull() ?: return@mapNotNull null
                        btnId to Offset(x, y)
                    } else null
                }.toMap()
            } catch (e: Exception) {
                emptyMap()
            }
        }

        fun launchGame(game: GameFile) {
            addToRecents(game)
            activeGamePath = game.path
            activeGameName = game.name
            activeGamePlatform = game.platform
            val platformCores = AVAILABLE_CORES[game.platform] ?: emptyList()
            val savedCore = prefs.getString("core_pref_${game.platform.name}", null)
            val coreToUse = savedCore ?: platformCores.firstOrNull() ?: "snes9x_libretro_android"
            activeGameCorePath = File(internalCoresDir, "lib$coreToUse.so").absolutePath
            buttonOffsets = loadLayout(game.name)
        }

        Scaffold(
            bottomBar = {
                if (currentScreen != Screen.GAME && currentScreen != Screen.ABOUT && currentScreen != Screen.HELP) {
                    NavigationBar {
                        NavigationBarItem(
                            icon = { Icon(Icons.Default.Home, contentDescription = "Home") },
                            label = { Text("Home") },
                            selected = currentScreen == Screen.HOME,
                            onClick = { currentScreen = Screen.HOME }
                        )
                        NavigationBarItem(
                            icon = { Icon(Icons.AutoMirrored.Filled.List, contentDescription = "Library") },
                            label = { Text("Library") },
                            selected = currentScreen == Screen.LIBRARY,
                            onClick = { currentScreen = Screen.LIBRARY }
                        )
                        NavigationBarItem(
                            icon = { Icon(Icons.Default.Add, contentDescription = "Import") },
                            label = { Text("Import") },
                            selected = currentScreen == Screen.IMPORT,
                            onClick = { currentScreen = Screen.IMPORT }
                        )
                        NavigationBarItem(
                            icon = { Icon(Icons.Default.Search, contentDescription = "Search") },
                            label = { Text("Search") },
                            selected = currentScreen == Screen.SEARCH,
                            onClick = { currentScreen = Screen.SEARCH }
                        )
                        NavigationBarItem(
                            icon = { Icon(Icons.Default.Settings, contentDescription = "Settings") },
                            label = { Text("Settings") },
                            selected = currentScreen == Screen.SETTINGS,
                            onClick = { currentScreen = Screen.SETTINGS }
                        )
                    }
                }
            }
        ) { innerPadding ->
            Box(modifier = Modifier.padding(innerPadding)) {
                when (currentScreen) {
                    Screen.HOME -> {
                        HomeScreen(games = gameList, recentGames = recentPaths.mapNotNull { path -> gameList.find { it.path == path } }.take(5), onGameClick = { launchGame(it); currentScreen = Screen.GAME }, onGoToLibrary = { currentScreen = Screen.LIBRARY })
                    }
                    Screen.LIBRARY -> {
                        LibraryScreen(games = gameList, favoritePaths = favoritePaths, onToggleFavorite = { toggleFavorite(it.path) }, onGameSelected = { launchGame(it); currentScreen = Screen.GAME })
                    }
                    Screen.IMPORT -> {
                        ImportScreen(onScanGames = { performScan() }, onScanCores = { val cores = scanCores(); android.widget.Toast.makeText(context, "Found ${cores.size} cores", android.widget.Toast.LENGTH_SHORT).show() }, onScanLayouts = { val layouts = scanLayouts(); android.widget.Toast.makeText(context, "Found ${layouts.size} layouts", android.widget.Toast.LENGTH_SHORT).show() }, coresCount = scanCores().size, layoutsCount = scanLayouts().size)
                    }
                    Screen.SEARCH -> {
                        SearchScreen(games = gameList, onGameSelected = { launchGame(it); currentScreen = Screen.GAME })
                    }
                    Screen.SETTINGS -> {
                        SettingsScreen(
                            prefs = prefs,
                            rootStorageDir = storageDir,
                            onReportBug = { android.widget.Toast.makeText(context, "Check logs in console", android.widget.Toast.LENGTH_SHORT).show() },
                            onGoToAbout = { currentScreen = Screen.ABOUT },
                            onGoToHelp = { currentScreen = Screen.HELP }
                        )
                    }
                    Screen.ABOUT -> {
                        AboutScreen(onBack = { currentScreen = Screen.SETTINGS })
                    }
                    Screen.HELP -> {
                        HelpScreen(onBack = { currentScreen = Screen.SETTINGS })
                    }
                    Screen.GAME -> {
                        // Create a safe directory for this game
                        val gameSafeName = remember(activeGameName) { 
                            activeGameName.replace(Regex("[^a-zA-Z0-9._-]"), "_") 
                        }
                        val gameSaveDir = remember(gameSafeName) { 
                            File(savesDir, gameSafeName).also { it.mkdirs() } 
                        }
                        GameScreen(gameName = activeGameName, platform = activeGamePlatform, gamePath = activeGamePath, corePath = activeGameCorePath, storageDir = storageDir, savesDir = gameSaveDir, layoutsDir = layoutsDir, buttonOffsets = buttonOffsets, onUpdateOffset = { id, offset -> buttonOffsets = buttonOffsets + (id to offset) }, onResetControls = { buttonOffsets = emptyMap() }, onBack = { saveLayout(activeGameName); currentScreen = Screen.HOME }, onTogglePause = { if (it) pauseGame() else resumeGame() }, onSaveState = { filePath -> 
                            android.util.Log.d("MainActivity", "Saving state to: $filePath")
                            saveState(filePath) 
                        }, onLoadState = { filePath -> 
                            android.util.Log.d("MainActivity", "Loading state from: $filePath")
                            loadState(filePath) 
                        }, onReset = { resetGame() }, onFastForward = { setFastForward(it) })
                    }
                }
            }
        }
    }
}
