# DRXexpandTM

DRXexpandTM is a 64-bit DirectX 11 visual-effects plugin for Euro Truck Simulator 2. It reads driver needs from TMRealWalk and adds effects intended to reflect the driver's condition:

- Hunger and thirst trigger vignette pulses as their values decrease. Very low needs enable a persistent vignette, and values at or below 5% add a subtle full-screen blur.
- Toilet need triggers short screen-shake pulses across configured value ranges. Below 5%, the shake repeats every five seconds.
- Shower values at or below 20% gradually desaturate the image and add subtle sweaty grime around the edges. Below 4%, three animated flies appear and `assets/fly.mp3` loops while the effect is active.
- When hunger reaches 0%, health decreases by 0.5 points per second. When thirst reaches 0%, health decreases by 1 point per second. If both are at 0%, the rates combine to 1.5 points per second.

Telemetry values are sampled every five seconds. Visual effects are rendered through a DirectX 11 `Present` hook. The `shaders` and `assets` folders must remain beside the plugin DLL.

## Compatibility disclaimer

**This plugin only supports TMRealWalk v1.00 Beta 5.2.** It reads TMRealWalk's fixed in-memory data layout, including the health value. Other TMRealWalk versions are not supported and may cause incorrect behavior or crashes. Make sure the supported TMRealWalk version is installed and loaded by ETS2.

## Installation

1. Build the project as **x64 Release**. The build output contains `DRXexpandTM.dll`, a `shaders` folder, and an `assets` folder.
2. Open the ETS2 installation directory. For a typical Steam installation, this is:

   ```text
   .../Euro Truck Simulator 2/bin/win_x64/
   ```

3. Create the `plugins` folder there if it does not already exist.
4. Copy `DRXexpandTM.dll` into `bin/win_x64/plugins/`.
5. Copy the complete `shaders` and `assets` folders beside the DLL, so the final layout is:

   ```text
   Euro Truck Simulator 2/
   └── bin/win_x64/
       └── plugins/
           ├── DRXexpandTM.dll
           ├── assets/
           │   └── fly.mp3
           └── shaders/
               └── vignette.hlsl
   ```

6. Ensure TMRealWalk v1.00 Beta 5.2 is installed and enabled, then start ETS2.

To uninstall, close ETS2 and remove `DRXexpandTM.dll`, `plugins/shaders/vignette.hlsl`, and `plugins/assets/fly.mp3`. If other plugins use either folder, remove only these plugin files and leave other files in place.
