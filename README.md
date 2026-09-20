RedmptionGUI
Made By Vr4se & GrandpaJoe
God-tier GUI mod lib for SlapLab on Meta Quest.
Unity 2022 · ARM64 · IL2CPP · No anti-cheat = ez pz 💀
---
Features
#	Feature	How it works
1	Fly	Kills gravity, right thumbstick controls altitude
2	ESP	Tracks all players — name, distance, HP
3	GodMode	Blocks every damage call, you literally cannot die
4	Knockout Instantly	Sets every player's HP to 0 in one shot
5	Noclip	Disables your collider, walk through anything
6	Spawn Prefab	Instantiates any Unity object by name
7	Gun	Physics force launcher — sends nearest player flying
8	Kick Gun	Massive knockback blast on ALL players
9	Ban Gun	Kicks nearest player from the session (host only)
---
Controls
Button	Action
Left Menu (Start)	Open / Close the GUI
Right B	Scroll down
Right A	Toggle / Activate selected
Right Thumbstick	Fly up/down
Right Trigger	Fire Gun
Left Trigger	Fire Kick Gun
---
How to Deploy
1. Build the .so
Push this repo to GitHub — Actions auto-builds it.
Go to Actions tab → latest run → Artifacts → download `libRedmptionGUI.zip`
2. Inject into APK
```bash
# Decompile
apktool d slaplab.apk -o slaplab_out/

# Drop the lib in
mkdir -p slaplab_out/lib/arm64-v8a/
cp libRedmptionGUI.so slaplab_out/lib/arm64-v8a/

# Repackage
apktool b slaplab_out/ -o slaplab_modded.apk

# Sign
java -jar uber-apk-signer.jar -a slaplab_modded.apk

# Sideload
adb install -r slaplab_modded_aligned_signed.apk
```
3. Patch the smali
Find the main Activity (`MainActivity.smali`) and add these 2 lines in `onCreate()` before `invoke-super`:
```smali
const-string v0, "RedmptionGUI"
invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```
---
Debugging
```bash
adb logcat -s RedmptionGUI:V
```
---
Notes
Ban Gun only works if you're the host (master client) of the Photon room
If hooks don't fire, dump `libil2cpp.so` with Il2CppDumper and check actual class names
No IL2CPP dump required to run — lib resolves everything at runtime
