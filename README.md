# Image Trainer for Xbox

A real neural-network trainer that runs on an Xbox Series S or X. You give it pictures from your phone, it learns from them on the Xbox's graphics chip, and you ask it to draw new pictures of the same kind. Nothing is simulated: the numbers on the chart are the real training error, and the pictures are really drawn by the network you are training.

The Xbox shows a controller-friendly screen on the TV. Your phone (or any browser on the same Wi-Fi) shows a full control page.

## First start, in three steps

1. Open the app on the Xbox. The **Phone** screen shows a web address and a square code.
2. On your phone, on the same Wi-Fi, scan the code with the camera or type the address into the browser.
3. On the phone page: **Pictures** > **Add pictures**, then **Train** > **Start**. The first drawings appear after a few seconds.

Without pictures the brain practises on four built-in shapes. That is fine for seeing how training works, but add your own to teach it something real. Pictures of one kind of thing (flowers, faces, logos) teach it much faster than a mix.

## The phone page

| Tab | What it is for |
| --- | --- |
| **Train** | Start, pause and stop. Live drawings, the error chart, speed, and what the numbers mean. |
| **Pictures** | Add as many pictures as you like, or a zip file of them. Each picture is shrunk on your phone before it is sent, so there is **no size limit** and nothing is lost to the old 16 MB limit. Select and remove pictures, or remove them all. |
| **Create** | Make new pictures from what the brain has learned: 1 to 16 at a time, with a choice of drawing steps. |
| **Saves** | Snapshots are save games for the brain. Save, load, rename and delete them. The Xbox also saves by itself while training. Download the brain as a file to keep on your phone, or add a snapshot file back in. |
| **Settings** | Learning speed, how it changes over time, warm-up, pictures per lesson, picture size, random flips/zoom/colour changes, a smoothed copy of the brain (EMA), how often to save, graphics chip or processor, and more. Every setting explains itself, and **Reset** puts the recommended values back. **Start over with a new brain** (small, medium or large) is also here, and **About this Xbox** shows the version, address and what the app is calculating on. |

## The TV screen and controller

| Button | What it does |
| --- | --- |
| **LB / RB** | Switch between Train, Settings, Saves, Pictures and Phone |
| **A** | The main action of the screen (Start/Pause, Apply, Load, Enlarge) |
| **X** | Make pictures (Train, Pictures) or Delete (Saves) |
| **Y** | Save now |
| **B** | Back / cancel |
| **D-pad or left stick** | Move around and change settings |
| **View** | Shows the Phone address and code, and goes back to training |

## Where things are saved

Everything (pictures, snapshots, settings) is kept in the app's own storage on the Xbox, so it is still there the next time you open the app and after an update. It is deleted if you uninstall the app. Download the brain from the **Saves** tab if you want a copy on your phone.

## Good to know (honest limits)

- The network is small on purpose (about 25,000, 80,000 or 250,000 dials) so it can learn on a console in minutes. Expect simple, soft pictures that capture colours, shapes and the general look of your pictures, not photo-realism.
- Keep the app open in front on the TV while it trains. If you switch to another app, the Xbox pauses it: the app saves first and carries on when you come back.
- The phone page has no password. Anyone on the same Wi-Fi who knows the address can open it, so use it on a network you trust.
- Training on the graphics chip is much faster than on the processor. If the graphics chip is not available the app says so and uses the processor.
- Very large collections (thousands of pictures) take a while to send. Keep the phone screen on until the upload says it is done.

## If something goes wrong

- **The page does not open:** the phone and the Xbox must be on the same Wi-Fi, and the app must be open on the TV. The page tries again by itself.
- **The page says it cannot reach the Xbox:** wake the Xbox, open the app, wait a few seconds.
- **The chart stays flat for a long time:** it has learned what it can at these settings. Try more pictures, a larger brain or a lower learning speed.
- **Everything looks wrong after a crash or a bad experiment:** open **Saves** and load an earlier snapshot.
- **Activity log:** at the bottom of the Settings tab, **About this Xbox** has a button that downloads the activity log (including any crash notes). Send it to whoever helps you.

## For developers

- Built by GitHub Actions (`.github/workflows/build-xbox.yml`, windows-2022, Visual Studio 2022, C++20, UWP / Xbox). The workflow copies the files below into `xbox-uwp/src`, compiles the GPU shaders with `fxc`, packs them into the app, and produces a signed `.msix`.
- `FMBrain.*` is the GPU and CPU network. `Trainer.*` is the training loop. `Dataset.*` keeps pictures. `Checkpoint.*` saves and loads snapshots. `AppCore.*` ties it together and is the only thing the web page and the TV screen talk to.
- `HttpServer.*`, `WebApi.*`, `WebHost.*` are the phone page's server. `WebUiAssets.h` is the phone page as a byte array (generated from `web/` by `web/build_web.py`). `TvUi.*` decides what the TV shows; `GamepadUI.*` and `Main.cpp` draw it and read the controller. `WebBridge.*` holds the few Windows-only helpers.
- The phone page uses the fonts Bricolage Grotesque and Lora under the SIL Open Font License; see `THIRD_PARTY_FONTS.txt`.
- Everything except the Windows-only files is compiled and tested natively on Linux (address and thread sanitizers, a real browser against the real server). The Windows-only files and the shaders are only built by the GitHub workflow.
