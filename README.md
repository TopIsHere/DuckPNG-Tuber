# Duck PNGTuber — OBS 32.0.x

A small native OBS source that switches between an idle PNG and a talking PNG
based on the audio level of an **existing OBS audio source**.

## Intended setup

    Microphone
        ↓
    Voicemod
        ↓
    Voicemod Virtual Microphone
        ↓
    OBS Audio Input Capture
        ↓
    Duck PNGTuber source (audio detector)

The plugin does **not** open the microphone itself.

## Security / privacy

This project intentionally contains no:

- network requests
- HTTP clients
- WebSockets
- browser/CEF code
- cookie access
- telemetry
- analytics
- automatic downloads
- installers

The source only uses OBS/libobs APIs and the PNG files selected in its settings.

## Build

You need a Windows OBS development/build environment matching your OBS build.
The official OBS plugin template currently uses CMake and Visual Studio 2022;
this project intentionally keeps the build files much smaller. See the OBS
plugin template for the supported build workflow:

https://github.com/obsproject/obs-plugintemplate

For an out-of-tree build, point CMake at the `libobs` CMake package from your
OBS build, for example:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH=C:\path\to\obs-studio\build_x64\libobs ^
      -DOBS_PLUGIN_OUTPUT_DIR=C:\path\to\OBS\obs-plugins\64bit

    cmake --build build --config RelWithDebInfo

If your OBS build exposes libobs somewhere else, change
`CMAKE_PREFIX_PATH` accordingly.

## OBS setup

1. Build the DLL.
2. Copy the resulting `duck-pngtuber.dll` into your OBS
   `obs-plugins\64bit` folder.
3. Put `idle.png` and `talking.png` in the plugin's data folder if they are
   not already copied by your build/install process.
4. Restart OBS.
5. Add **Duck PNGTuber** as a source.
6. Set **OBS audio source** to the name of your Voicemod Audio Input Capture
   source.
7. Adjust **Talking threshold** until the mouth only opens while speaking.
8. Adjust **Mouth release delay** if the mouth closes too quickly.

## Important note about the supplied images

The two supplied images are full 1405×1119 RGBA pictures of the room + duck.
They are not transparent cut-outs of the duck.

So this plugin will display the full rectangle exactly as supplied. For a
normal PNGTuber overlay on top of gameplay, use two transparent PNGs containing
only the duck.
