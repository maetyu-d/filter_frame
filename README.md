# ff::

A standalone JUCE/C++ spectral app built around a parallel SuperCollider filterbank.

The app divides the audible spectrum into perceptually scaled frequency bands from roughly 20 Hz to 20 kHz. It can show broad octave groups or finer third-octave bands while keeping one shared underlying band structure. Each band has its own SuperCollider code, level controls, mute/solo, pan, and metering.

## Build

```sh
cmake -S . -B build -DJUCE_PATH=../Granny/JUCE
cmake --build build --target filter_frame -j 6
```

If your JUCE checkout is somewhere else, pass that folder as `JUCE_PATH`.

## Run

The built macOS app is:

```text
build/filter_frame_artefacts/Debug/ff.app
```

## SuperCollider

`ff::` keeps one persistent `sclang` bridge alive, boots the SuperCollider server once, and sends band code plus band-isolation settings over OSC. Each band is filtered before it reaches the app mixer/meter path, so band rows behave like isolated spectral processing spaces.

The app auto-detects `/Applications/SuperCollider.app/Contents/MacOS/sclang` on macOS, then falls back to `sclang` on the shell path. Audio device settings live in the File menu.
