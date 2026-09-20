# RedmptionGUI — Build & Deploy Guide
**By Vr4se & GrandpaJoe**

## Requirements
- Android NDK r25c+ (`arm64-v8a`)
- Linux/Mac/WSL build machine
- `apktool`, `uber-apk-signer.jar`, `adb`

---

## Step 1: Build the .so

```bash
cd RedmptionGUI/
$NDK_PATH/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# Output: libs/arm64-v8a/libRedmptionGUI.so

# Optional: strip for smaller size
$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip \
    --strip-all libs/arm64-v8a/libRedmptionGUI.so
```

---

## Step 2: Inject into SlapLab APK

```bash
# 1. Decompile
apktool d slaplab.apk -o slaplab_out/

# 2. Drop the lib in
mkdir -p slaplab_out/lib/arm64-v8a/
cp libs/arm64-v8a/libRedmptionGUI.so slaplab_out/lib/arm64-v8a/

# 3. Patch smali — find the main Activity (usually MainActivity.smali)
#    Add these lines in onCreate() BEFORE super.onCreate():
#
#    const-string v0, "RedmptionGUI"
#    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
#
#    Full example:
#    .method protected onCreate(Landroid/os/Bundle;)V
#        .locals 1
#        const-string v0, "RedmptionGUI"
#        invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
#        invoke-super ...   <- original super.onCreate line

# 4. Repackage
apktool b slaplab_out/ -o slaplab_modded.apk

# 5. Sign
java -jar uber-apk-signer.jar -a slaplab_modded.apk

# 6. Install on Quest (make sure developer mode is on)
adb install -r slaplab_modded_aligned_signed.apk
```

---

## Controls (In-Game VR)

| Button | Action |
|--------|--------|
| Left Menu (Start) | Open / Close GUI |
| Right B | Navigate menu down |
| Right A | Toggle selected feature |
| Right Thumbstick Y | Fly up/down |
| Right Trigger | Fire Gun (when Gun is ON) |
| Left Trigger | Fire Kick Gun (when Kick Gun is ON) |

---

## Features

| Feature | Default | Notes |
|---------|---------|-------|
| Fly | OFF | Gravity off, right stick = altitude |
| ESP | OFF | Shows all players: name, distance, HP |
| GodMode | OFF | Blocks ALL incoming damage |
| Knockout | — | One-shot: kills all players instantly |
| Noclip | OFF | Walk through walls (collider off) |
| Spawn Prefab | — | Instantiates a named Unity prefab |
| Gun | OFF | Physics force launcher at nearest player |
| Kick Gun | OFF | Big knockback blast on all players |
| Ban Gun | — | Kick nearest player from session (host only) |

---

## Debugging

```bash
# Live logcat from the mod
adb logcat -s RedmptionGUI:V

# Common log tags to watch
adb logcat -s RedmptionGUI:V Unity:V
```

---

## Tuning for SlapLab Specifically

SlapLab uses Unity 2022 IL2CPP with Photon Pun 2 for networking.

If `IL2CPP::GetMethod` fails to find classes on first run:
1. Do an Il2CppDumper on `libil2cpp.so` from the APK
2. `grep dump.cs` for the actual PlayerController class name
3. Update the class name strings in `SlapLab::Resolve()` inside `redemption_gui.cpp`

Common alternate class names in SlapLab-style games:
- `SlapController` instead of `PlayerController`
- `SlapHealth` instead of `HealthManager`
- `LobbyManager` instead of `RoomManager`

After finding actual names → rebuild → redeploy.

---

## Notes on Ban Gun

Ban Gun calls `PhotonNetwork.CloseConnection(player)` which only works when
you are the **master client** (host) of the Photon room. If you're not the host,
it'll silently fail. Host randomly assigned at room creation — rejoin if needed.
