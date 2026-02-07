# Nova Emulator

A multi-platform Android emulator frontend built with Jetpack Compose and libretro.

## Features

- **Multi-platform support**: Play SNES, Genesis, PS1, N64, GameCube and more
- **Save states**: Save and load game progress with 5 slots per game
- **Custom controls**: Editable touch controls with multiple layout styles
- **Cheat codes**: Support for Game Genie and Pro Action Replay codes
- **Fast forward**: Speed up gameplay with the FF button

## Building

```bash
cd android-app
./gradlew assembleDebug
```

## Installation

1. Build the APK using the command above
2. Install `app/build/outputs/apk/debug/app-debug.apk` on your Android device
3. Copy your game roms to `/storage/emulated/0/Emulator Games/`
4. Copy libretro cores to `/storage/emulated/0/Documents/Nova/cores/`

## Project Structure

```
nova-emulator/
├── android-app/          # Main Android app
│   ├── app/src/main/
│   │   ├── java/com/nova/  # Kotlin source
│   │   ├── cpp/              # Native C++ code
│   │   └── res/             # Android resources
└── examples/             # Example cores, layouts and resources
```

## NovaCore Website (Coming Soon)

A companion website for optimized cores, layouts and resources:
- Optimized libretro cores
- Custom control layouts
- Cheat code databases
- BIOS files

## License

MIT License
