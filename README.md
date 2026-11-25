Universal Recorder
==================

A Windows screen recorder. It saves MP4 files with sound from your computer and microphone.

Usage
-----

Download `Universal-Recorder-x64.exe` from the [releases page](https://github.com/gonbenvindo-debug/universal-recorder/releases/latest) and run it.
Press `Ctrl+Shift+R` to start recording. Press it again to stop.
The dot next to the clock is yellow when idle and red while recording.

Right-click the dot to change the recordings folder, frame rate, resolution or keyboard shortcut.
If you have more than one monitor, you can choose which one to record.

Build
-----

Install the Visual Studio C++ build tools and Windows SDK 10.0.26100 or later.
Run this command from the source directory.

    build.cmd x64

The executable is written to the same directory.

Based on [wcap](https://github.com/mmozeiko/wcap/tree/aa25ccb806d7a6e1c0bfdcca863aabcd8e9badfa) by Martins Mozeiko. See [LICENSE](LICENSE).
