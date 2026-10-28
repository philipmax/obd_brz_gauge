# Changelog

English | [简体中文](CHANGELOG.md)

Newest first. Behavior and flashing changes only; pure refactors and comment
cleanups live in the git history.

---

## Temperature hysteresis fix

- Confirm temperature jumps of 16–19°C and correct rounding of negative temperatures.


## Theme video switching reliability

- Hand off reader tasks without allocating a new DMA buffer while the old reader still owns one; clear video paths when the SD card is unavailable or an asset name is invalid to avoid reading uninitialized data. Add reader handoff and invalid-asset regressions and keep the relevant host-test assertions enabled in Release builds.

## I2C main-bus error handling

- Serialize device-cache registration and return errors when registration fails or the cache is full. Register writes now use a single multi-buffer transaction instead of task-stack space proportional to payload length. Pin definitions remain unchanged.

## OTA timeout and session protection

- Synchronize OTA state and 64-bit activity time so an old timeout task cannot stop a newly opened session; recheck activity before timeout shutdown. Discovery, device-info and authenticated requests refresh the timer. Report startup failure if the monitor task cannot be created, preserving nonblocking lifecycle and USB handoff protection.

## Protect data after upload errors

- Abort damaged upload sessions after SD write, sync or rollback-seek failures to prevent further writes at the wrong position; a failed digest cannot be verified again or committed. A network receive failure can still retry from the current chunk's start.

## Protect SD single-file replacement

- Preserve an `.obd-bak` backup while replacing an existing file. Restore the old file if the new rename fails, and retain the backup for manual recovery if restoration also fails. Existing backups are never overwritten; FAT power-loss atomicity is still not guaranteed.
- SD initialization uses static mutexes, and retries can complete a partially initialized pair.

## Signal timestamp consistency

- Sample time under the signal-store lock for reads and writes, preventing a new value committed while waiting for the lock from appearing stale. Regression cases cover single, batch and theme snapshots as well as millisecond-counter wraparound.

## BLE polling task shutdown protection

- ELM327 callbacks wake the polling task through a persistent static semaphore, including during BLE shutdown, so late callbacks cannot use a deleted task handle. The serial polling flow remains unchanged.

## Transparent custom gauge tracks

- Runtime theme arc/bar elements accept optional `bg_opacity`, affecting only the track while retaining an opaque foreground. Older themes default to 100%. Editor preview/export are synchronized and parser plus real LVGL style host tests are added. Hardware checks confirm full-screen image coverage, video alignment, stable page-entry sizing and background transparency.

## Pressure and high temperature fixes, acquisition and video timing

- Add generic MAP relative pressure and preserve profile vacuum readings; extend coolant/oil limits to 255°C and update catalog/import profiles. Prioritize RPM/speed independently, back off failed requests and remove duplicate speed-cache smoothing. Fullscreen video decodes ahead and submits alternating DMA strips on deadline, including OBV2 pause, stop and catch-up paths. Host tests pass and release firmware is built.

## Clear Installed Vehicle Profiles Over Web USB

- Add explicitly confirmed `vehicle.clear` management requests to remove installed config JSON and SD mirrors, then reboot with builtin Generic. Retain the running compiled profile, media and templates. The web flasher adds scope information, confirmation, old-firmware guidance and separate restart retry; partial deletion failures are not reported as success.

## Fix Automatic Protocol Detection With Replies but No Data

- Fix compact `ATH1 + ATS0` CAN responses and byte-separated 29-bit response headers. Map standard OBD request headers to 29-bit addressing on protocols 7/9, and retain native addressing with `ATH0` on J1850. Extend K-line initialization deadlines and wait for `>` after timeout interrupts. Cache automatic results only for the session, preserving the user's Automatic selection and probing again after sustained decode failures. Add host regression cases for response formats, slow initiation and recovery; subsequent host regression checks completed.
- Preserve detected protocols across profile `ATSP0`, query `ATDPN` after adapter auto selection, and reject probes whose `ATSP` failed. Map standard receive filters with 29-bit addressing and support Mode 01 fallback for native 29-bit requests. Drain ordinary timeouts before continuing and reconnect after write failures. Monitor windows temporarily enable `ATS1` and restore `ATS0`; BLE supports writes without response and isolates other characteristic notifications. Add corresponding regression cases.

## Fix CAN Request Headers on ISO K-Line Protocols

- Ordinary OBD requests on ISO 9141-2 / ISO KWP2000 no longer send CAN-style `ATSH` commands, including for imported profiles with an explicit `7E0`; they retain ELM327 native addressing and use `ATH0` to hide K-line response headers. Add a host regression test.

## Fix Theme Video Stalls That Made Live Data Drop to Zero

- Run theme-video SD reading and JPEG decoding below LVGL and OBD polling priority so both cores cannot be monopolized by video workers, which could stop UI handoff on one frame and let live signals become stale and render as the default `0`. Fullscreen boot video keeps its dedicated high-priority path.

## Increase the video JPEG frame limit

- Raise the OBV1 / OBV2 JPEG frame limit from 48 KiB to 128 KiB and update the offline converter. Retain the OBV2 1.25 MiB one-second window plus wrap headroom and the OBV3 256 KiB combined-frame cap. Frames above the old limit require updated firmware; existing animations remain compatible.

## Fix early boot-animation exit and missing sweep numbers

- Skip expired intermediate OBV2 frames while retaining the first and last, instead of aborting after three consecutive late frames. Preserve the source timeline and keep real read/decode errors fatal. Add slow-decode, stalled-reader and decode-failure regressions.
- Theme numeric labels, including custom fonts, share the same simulated 12 standard OBD signals as rules during the gauge sweep and restore live data afterward without overwriting it. Add stepped label-sweep, font and restoration regressions.

## Fix excessive theme-video frame skipping and unnecessary alpha compositing

- Play looping videos on the source timeline and cap FPS only when `target_fps` is explicitly set. Remove the default 24 fps ceiling and coarse frame-skipping tiers, recheck lateness after UI waits and ready display refresh as each frame is accepted. Keep fully opaque OBV3 frames in RGB565 while preserving real transparency and per-frame format changes; fix overlapping bytes during in-place alpha expansion. Add pacing, UI-wait, alpha-validation and frame-format regressions, plus diagnostics for decoder publication rate / cost and UI wait time.

- Raise the core 0 SD reader to priority 5 so continuous LVGL redraws at priority 4 do not starve it. Use bulk file-descriptor `read` calls through a 16 KiB internal DMA read-ahead buffer that survives frame-padding skips, avoiding byte-by-byte stdio reads and repeated prefetch in the target newlib. Once a theme has started, resume on the next available frame after an underrun instead of repeatedly refilling a 400 ms watermark. Add slow-reader per-frame resume and cross-block read / seek regressions, plus logs for active SD throughput, average / maximum frame read time and UI frame handoff intervals.

## Improve buffering and slow playback for looping theme videos

- Increase looping theme-video prefetch from approximately 1.25 to 2.5 seconds and the ring ceiling from 1.25 to 2 MiB. Reserve PSRAM for both display buffers, the alpha plane and other UI work, shrinking the cache when memory is limited. Preserve the approximately 400 ms startup watermark and existing value-driven and OBV2 boot-animation policies.
- Remove the five-second freeze during slow decoding and keep updating by skipping late frames on the source timeline. Add regressions for sustained slow playback, pacing recovery and ring memory budgets.

## Unify and validate firmware and Android releases

- Watch `main` for firmware releases by default and accept development-branch artifacts after fast-forward or normal merges; the build branch need not match the watched branch. Validate artifacts before updating the manifest, and match the complete NUL-terminated build tag. Legacy raw boot media is optional and stale published copies are removed when absent. Automatic source commits exclude both CHANGELOG files, and pushes explicitly target origin and the current release branch.
- Web and App share one firmware manifest and verify download/cache sizes and SHA256. Complete web flashing verifies four files before writing; App upload rechecks current hardware. The server validates source/artifact commit relationships, persists successful state and retries failures. Android uses a separate APK build manifest binding version, source, signer and file digest.

## Unify App installation over Wi-Fi and USB

- `/ota/theme` accepts new `OBDT` bundles or legacy `theme.bin` based on the file header, preserving older clients; invalid bundles never fall back to legacy import.

- Use a nonblocking lifecycle guard for OTA start/stop so USB takeover, UI exit and idle cleanup cannot release the server twice. Refuse takeover or exit reboot while busy or after stop failure, allowing a later retry.

- Themes, boot animations, vehicle installation/activation and theme deletion try Wi-Fi first, releasing the network binding and falling back to USB on Android; iOS remains Wi-Fi only. USB can use `hello` / `ota.stop` to stop OTA before takeover. Strictly validate preparation, chunk offsets, final success and business responses. Themes use journal-protected OBDT installation, and OBV2 is synced and validated before replacing the old animation; failure preserves current content. Firmware automatically reboots after media installation without an App duplicate; reboot failure after successful deletion or activation only prompts a manual restart.

## Simplify vehicle protocol and PID editing

- The web vehicle editor accepts standard OBD2, local JSON or community profiles, lists the 9 built-in standard items and PIDs, and supports adding data and editing request headers and formulas while generating firmware RPN. New items default to one-second intervals and imported profiles retain their schedules; merging standard data preserves custom or disabled items and raw manufacturer requests, CAN and fallback fields.

## Simplify theme-editor workflows and fit the available space

- Add labeled tools, step-specific guidance and prebound RPM / speed / coolant starters. Prioritize appearance properties, collapse advanced position and internal ID fields, and allow static text to bind directly to vehicle data while preserving selection across modes. Wrap toolbar controls onto separate rows, collapse alignment tools and fit the canvas when the window or simulator changes; add browser regressions for responsive layout, starter undo/redo and data binding.

## Value-driven theme video animations

- Add loop / follow-value configuration, signal ranges and steps, and aligned video/numeric timelines to video import and existing layer properties. Editor simulation and firmware sweep select OBV1 frames by value, supporting backward movement, endpoint clamping, holding missing data, and existing opacity / visibility / asset events. Use indexed seeking and on-demand decoding while retaining one active decoder; add parser, page-binding, player-concurrency and browser regressions.

## Preserve videos in theme drafts and project backups

- Save projects and actual video files together in browser IndexedDB and include all available media in editable `.gaugeproj.zip` backups. Open legacy JSON and relink missing videos under their original identifiers while retaining layers and events. Ignore video records without layer or event references during theme export, so deleted layers no longer cause missing-file errors.

## Fix internal identifier collisions when reimporting theme media

- Allocate unique internal identifiers for same-name video imports, including retained media after layer deletion and imports completed during conversion. Preserve existing videos and event references without manual file renaming. Allocate image names against the current project and undo video media and layers together.

## Simplify the theme editor and diagnose sweep rules

- Remove the separate resource-library entry while retaining direct image/video imports in the toolbar. Open metadata from the theme name and custom fonts from text properties. Log page rule/target counts and sweep active-rule changes, and add a real-LVGL regression for video-opacity rules during gauge sweep.

## Show disabled web preview rules and their actual settings

- Distinguish disabled rules from waiting rules in previews, display trigger conditions, current simulated values and action targets, and provide an explicit enable button. Event-list status dots only indicate state and no longer toggle enablement when clicked, avoiding accidental disabling while selecting a rule.

## Fix debounce timing for instant events in web previews

- Fix theme rules containing only instant actions and a debounce remaining pending in web previews, preventing opacity changes. Event and preview workspaces keep the debounce clock advancing, so manual simulated RPM or other data changes can still trigger and restore image and video opacity while the timeline is paused. Timer triggers continue to use the preview timeline.

## Wait for startup during USB theme replacement and support legacy replies

- Fix immediate web theme-install failure after the gauge restarts from flashing mode while its theme is still loading and legacy firmware only returns `theme delete failed`. Cleanup shows a waiting message and retries every 500 ms with a two-minute waiting budget, including specific legacy errors without a retry hint; explicit non-retryable errors and communication failures stop immediately. Firmware reports the failure stage and ESP error code, distinguishing theme-preparation contention, missing SD and deletion failure.

## Automatically clear the current theme before USB installation

- Local and community theme installation validates the new bundle, then detects and deletes the current theme’s `theme.bin`, rules, images, fonts, videos and saved selection over USB. With no current theme, upload proceeds directly; restart happens only after enabling the replacement. Cleanup retries boot preparation or lock contention explicitly marked retryable by firmware and stops on deletion failure. Other themes and the shared boot animation remain. If upload fails after deletion, the next restart uses the built-in default theme.

## Allow boot-time storage contention during local USB uploads

- Fix immediate local USB theme-upload failure when an existing theme reads resources or background SD jobs temporarily occupy storage. Firmware media BEGIN waits up to one second for the lock; the web uploader retries BUSY with fresh sequences every 500 ms and a 30-second waiting budget for themes and boot animations. Timeout and CRC transport retries retain their sequence; persistent contention still reports failure. Theme installation automatically clears the current theme.

## Theme video handoff and opacity events

- Fix lower video playback after the upper video is hidden or reaches zero opacity, and prioritize video posters in the image cache. Images and videos support base opacity; events support target opacity, transitions in seconds and restoration on release. Summarize trigger, action and restoration in the event panel. Browser previews support multiple videos, retain posters until the first decoded frame and keep existing content after bad frames. Firmware still shares one JPEG decoder and cannot crossfade two simultaneously decoded videos.

## Web video conversion progress

- Show conversion progress bars, processing stages, percentages and available frame counts when importing theme videos or transcoding boot-animation community uploads. Use an indeterminate bar when the total is unknown. Theme-video progress covers local decoding, frame encoding and canvas-layer generation, and closes after success or failure.

## Disable the backlight early to hide panel initialization

- Drive the backlight GPIO inactive at application entry, including SD/configuration bring-up; initialize PWM before LCD reset/display-on, and continue enabling the backlight only after the first Logo frame finishes DMA and a 50 ms panel-scan settling delay. Power-on levels during ROM / bootloader execution still require cold-start hardware validation; theme and animation formats are unchanged.

## Keep the Logo and boot animation visible while preparing transparent-image themes

- Turn on the backlight after the Logo's first LCD flush completes. Read and convert the default theme page's images in the background. Keep the Logo until the first fullscreen boot-video frame reaches the LCD, so slow SD prebuffering or JPEG setup cannot switch to an empty screen. Guard the LVGL refresh callback during fullscreen playback to prevent page/layout invalidations from clearing the panel or overwriting borrowed DMA buffers. If the video ends before theme preparation, release the player and return to the Logo until the page is ready instead of freezing on the final frame. Log OBV2 prebuffering, first-frame delivery, theme preparation/page creation and slow image-read/conversion times; raise the preparation task priority while keeping it below video decoding. Decode failure or interruption restores the Logo, and failed theme preparation falls back to a built-in page. Block theme unload/reload and USB resource replacement/deletion during preparation. Existing theme packages do not need to be exported again.

- Replace `_IONBF` + `fread` theme-package streaming, which refills byte by byte with the target libc, with bulk file-descriptor `read` calls through internal DMA buffers. On the same device, reading the same 518400-byte RGBA image dropped from about 11.7 seconds to 140 ms; conversion took about 100 ms. Existing packages need no re-export. Add read-phase and playback-completion/theme-entry logs to distinguish SD waits from playback teardown.

- Backport the upstream ESP-IDF SPI bus-lock wakeup fix for a cross-core waiter missed at LCD DMA exit, which could freeze boot video near its end. Generate a build-local patched copy without modifying the installed SDK. Release the low-speed LCD probe IO to avoid leaking its handle and lock slot. Preserve resolution, timing and every video frame; the same device completed all 100 frames and entered the transparent-image theme on 20 consecutive USB resets.

## Fix apparent gauge-sweep jumps caused by theme color events

- Arc and bar `set.color` actions now change only the indicator/fill color while preserving the original track, for immediate changes and fade/keyframes/strobe animations; exiting rules restores the base color. Sweep events remain active and match the web preview. Existing theme packages require only a firmware update, with no re-export.
- Fix arc progress incorrectly inheriting the 8191 pixel-coordinate limit; restore the full 0–10000 value range so both sweep and live readings reach full scale.

## High-quality SD boot animation with per-frame timing

- Fix valid OBV2 uploads being rejected when validation read the SD file before its updated directory size was synchronized; sync failures now return an install error and preserve the previous animation.
- Web USB boot-animation installs now replace other old formats: committing OBV removes legacy raw/JPEG, committing BIN/raw removes OBV/JPEG, and same-format updates replace the existing file. Transfer interruption or validation failure skips old-file cleanup; cleanup failure returns an error. Theme resources are unaffected.
- Local web boot-animation upload accepts common video formats; non-H.264 MP4 inputs use FFmpeg WebAssembly in the browser and are then encoded to the firmware-supported OBV format. Source video stays local, source timing is retained, and frame, buffer and file-size checks still apply.
- Convert local H.264 MP4 in the browser to aspect-preserving 360×360 Q90 baseline JPEG/OBV2 with source frame timing, install shared SD `boot.obv` after USB command `7` validation, and prebuffer about one second with whole-frame decoding and double-buffered DMA strip output. Report frame/file/buffer overflow; stop after three consecutive display-deadline misses. Actual frame rate requires device testing. Add OBV2 parser/installation-validation tests and inspection support for both formats; retain legacy BIN/raw and OBV1. Replace the previous fixed low-FPS/30-second local conversion limits; source files stay local.

## Boot animation handoff and local MP4 upload

- Compute legacy raw playback duration from frame count and FPS. Prebuild the scan page when there are no theme pages and no saved device binding, starting scanning only once shown. The web flasher converts local MP4/M4V/MOV/WebM videos in the browser before USB installation; one video may be up to 30 seconds.

## Use USB for all web device operations

- Web firmware flashing, theme/boot-animation uploads, deleting the current theme, vehicle installation/activation and reboot all use USB. Remove device address and OTA token inputs and the gauge hotspot requirement. Firmware adds management command `6` with SHA-256 verification and JSON replies; duplicate END replays the cached result. Theme deletion removes the current SD package and selection; vehicle installation never activates or reboots automatically. Upgrade older firmware first; App WiFi APIs remain compatible.

## Trigger theme rules during gauge sweep

- During the Bluetooth-connect sweep with a theme page on screen, theme rules temporarily simulate all 12 standard `obd.*` signals and mark them as changing, so threshold and `change` rules for any standard OBD signal can respond. Rule input returns to real OBD data when the sweep ends; simulated values are never written to vehicle data. Vehicle-specific `veh.*` signals remain live.

## Protect SD temporary files during USB uploads

- USB theme and boot-animation transfers hold the storage lock through reception, verification and commit, preventing startup cleanup from deleting an active `.part` file. Abort, failure and timeout release the lock; theme installation preserves the LVGL → storage lock order.

## Delete themes and install vehicles on the web flasher

- Provide “Delete current theme” through USB to remove the current theme SD directory and companion resources, clear the selection, and restart. Restore community access and local vehicle JSON validation, installation and a separate enable-and-restart action; no device address or OTA token is required.

## Unified web resource sources and legacy theme compatibility

- Both themes and boot animations can be installed from the community or local files. Themes accept compressed ZIPs, legacy BINs, and companion rules/videos; new community publications store complete ZIPs. A legacy BIN alone cannot restore missing rules or videos, and declared missing videos block installation. Reconnect after a complete firmware flash before installing SD resources.

## Theme ZIP export and one-click USB install/activation

- The editor exports BIN and SD resources together as one ZIP. The USB flasher accepts a local ZIP, installs it on SD with rules, enables it, and restarts. Firmware adds theme-bundle transfer, replacement rollback and boot recovery, and fixes USB ACK CRC; a firmware update is required before first use.

## Restore vehicle and theme entries in Settings

- Restore VEHICLE and THEME at their original coordinates: the VEHICLE button opens the vehicle page, and the THEME roller selects a theme and restarts to apply it. The SD-card management entry remains hidden.

## Unified vehicle selection through the roller

- Remove the UNDO and GENERIC buttons from the vehicle page. Generic OBD2 and profiles imported through the web UI, App or SD card now share one roller; select one and press USE to apply it.

## Compact Settings and a single Vehicle button

- Replace the vehicle row with one centered Vehicle button, reduce Settings fonts, rollers and buttons, and remove the bottom navigation hint while retaining swipe-down for multi-gauge and left/right to return. Profile and state remain on the vehicle page. Correct the UNDO explanation: it restores the saved previous profile rather than simply cancelling a selection.

## Vehicle-community direct-install allowlist

- The device exposes CORS only for the vehicle install, select and reboot POST endpoints, still requiring an OTA session token. Installation never selects a profile automatically; selection and reboot are separate confirmations. Other device APIs remain closed to CORS; SD JSON import remains available when browsers block direct access.

## Store all custom theme and animation resources on SD

- Read images, fonts, layouts, theme videos and independent boot animation from SD. APPLY validates and saves selection before reboot; OTA media streams to SD without Flash media caching. Remove the old 4 MB package limit and budget RGB565 RAM caches; boot without a card uses the built-in UI. Boot video reads only the common SD boot directory and is independent of theme selection. USB flashing excludes media; copy release animation to SD `/OBD/BOOT/bootmedia.raw`. Preserve layout v2 legacy partitions for migration/reservation and existing SD resources.

## Single-line vehicle row and SD card button in Settings

- Put the VEHICLE label and profile button on one row, keep the name on one line with room for its status badge, and avoid wrapping overlap. Add an SD CARD button for storage, remove the swipe-up entry from Settings, and adjust widget spacing.

## Default profiles without a protocol to standard OBD-II selection

- When a vehicle JSON omits `bus.proto`, reuse a saved/already-detected protocol or
  auto-detect when none is available. Profiles requiring a fixed protocol can still
  declare values such as 6 or 7 explicitly.

## Append unique records directly in ESP-NOW master snapshots

- Initial master collection avoids duplicate-ID searches, removing 903 comparisons for
  43 ordinary records while retaining linked RPM replacement, validity masks and v5/v6
  bytes. Static RAM and collector stack usage are unchanged; each of three firmware
  images grows by 144 bytes. Desktop full collection improves; device timing and wireless
  throughput remain to be measured.

## Signal ID lookup avoids static table scans

- Generate exact ID-to-slot mapping from the registry, removing static table scans from
  ESP-NOW receive processing while preserving vehicle registration counts and locking.
  All 16-bit IDs, reordered/sparse registries and three firmware builds pass; target code
  size is unchanged with 4 fewer literal bytes. Desktop RPM-only lookup regresses slightly;
  actual receive timing remains to be measured on the device.


## Reuse chip ROM for ESP-NOW v6 CRC

- v6 encode/decode CRC uses ROM, preserving protocol bytes and receive decisions while
  removing eight bit iterations per byte. No RAM lookup table is added; target codec
  code/literals shrink by 126 bytes. Verify all CRC state transitions, full-frame
  comparison and three firmware builds; device timing and wireless throughput remain unmeasured.

## Remove full-image temporary conversion for identity-size pointers

- Runtime theme pointers crop/convert directly at identity size, removing a 518,400-byte RGBA
  temporary request for 360×360 artwork. Untinted RGB565 also removes a 259,200-byte persistent
  pixel copy. Update budget estimates while preserving pixels, pivots and cache-reference
  lifetime. Verify 1,006 artwork cases, three firmware builds and three negative controls;
  device timing remains unmeasured.

## Reduce temporary axis-weight memory during theme scaling

- Scaling indices/weights reserve tighter capacity by mode, preserving allocation counts,
  tap order and image bytes. A mixed-scale example with a maximum-size identity axis requests
  24,594 fewer bytes. All 4,190,209 axis pairs and 10,464 image cases match. Target function
  code grows by 56 bytes; actual artwork load time still needs device measurements.

## Remove per-channel floorf from theme pointer resampling

- Theme pointer resampling quantizes channels with bounded integer conversion while preserving
  float rounding and image bytes. The target resampler removes four library call sites and
  168 bytes of code/literals. All Binary32 patterns and 10,448 image cases match; initial
  artwork load time and page FPS still need device measurements.

## Correct identity-unit writeback rounding boundaries

- Use hardware integer/float conversions to preserve original double rounding for identity-unit
  vehicle samples, fixing early carry just below half ties and at large integers. Other factors,
  filtering and polling order are preserved. Add full Binary32 rounding and real-writeback
  state/callback comparisons; remove `floorf` from the target identity path. Device timing remains unmeasured.

## Remove periodic composite frame handoff polling

- Frame publication/resume notifies the LVGL task, replacing the per-session 10 ms timer. Pause
  retains pending frames; late notifications after stop/replacement carry no retired object pointer.
  Ordinary timer deadlines, fullscreen refresh and DMA lifetimes are preserved. Each normal/statistics
  configuration passes 47 scenarios; all 27 negative controls are rejected. Actual gains need device measurement.

## Stop empty handoff checks while composite video is paused

- Pause/resume the composite 10 ms LVGL handoff timer with playback, retaining pending frames
  until resume. Stop while paused still releases resources; fullscreen refresh is not resumed
  prematurely. Real-player tests expand to 36 scenarios; 15 negative controls and three target
  builds pass. CPU/power/actual wakeups and resume latency still require device measurement.

## Remove periodic timeouts from video condition waits

- Video pause, full-ring, header/watermark and UI-handoff conditions use existing event notifications;
  resume/stop actively wake workers. Frame deadlines, LCD DMA timeouts, buffer lifetimes and
  LVGL/theme-owner timers remain. Add 28 real-control scenarios and 10 negative controls; three
  target builds pass. Actual CPU/power/FPS gains remain unmeasured.

## Skip the ineffective storage task without an SD slot

- Use the SD slot configuration to omit a background poller and its 8192-byte task stack when card
  jobs can never run. Boot migration, config mounting, theme recovery and busy reporting remain.
  Configured empty slots still detect insertion, with import/OTA/busy-retry behavior unchanged.
  Add dual-configuration real-service tests and CI; three target builds pass. Actual heap/CPU/power
  gains remain unmeasured.

## ESP-NOW linked ramp reuses the broadcast worker

- Remove the independent ramp task that woke every 20 ms even when idle. The broadcast worker shares
  one configuration/ramp sample for local and slave output, retaining the timeline and idle 10 Hz /
  TEST 50 Hz broadcasts. TEST events shorten idle waits without bypassing the 20 ms deadline; repeated
  requests restart the timeline. OTA and task-creation failure close admission so late requests cannot
  restart TEST. Add real worker/control/OTA checks and bilingual performance accounting; target builds
  pass, with device CPU/power/stack margins still unmeasured.

## RaceChrono event- and subscription-deadline wakeups

- The stream waits for the earliest subscription deadline instead of waking every 20 ms while
  disconnected, notifications are off, deny-all is active, or only retired channels are subscribed.
  Connection/filter/service-ready/OTA shutdown events use a boot-lifetime static wake token.
  Invalid samples are rate-limited without claiming send intervals; disabling notifications during
  a snapshot also leaves intervals unclaimed. Wire format and the minimum 20 ms sending interval
  remain unchanged; other intervals no longer quantize to the old checking grid. Added deterministic
  actual-worker/GATT/hub tests, deadline comparisons and bilingual performance docs. Device CPU,
  power and wireless gains remain unmeasured.

## Theme page active-page and binding-item caching

- `theme_tick()` caches the current theme page and, after the first refresh, visits only non-image elements with a `data_source`; page transitions, deletion, and allocation failure retain safe fallbacks without changing theme-rule or page behavior. Device frame-time gains remain unmeasured.

## Integer delivered-unit conversion

- `signal_store` uses integer fast paths for common oil-pressure and voltage unit conversions,
  preserving units, rounding, validity, and link-gating semantics; other signals and unmatched
  metadata retain the generic formula. Added full-boundary, randomized, public-snapshot, and
  ASAN/UBSAN comparisons; target code checks confirm that valid paths remove double multiply/divide,
  while device gains remain unmeasured.

## Theme images classify round-panel visibility by row

- RGBA image planning classifies the window first, then computes visible pixel spans and
  first/last nontransparent pixels per row, reducing circle tests and bounding-box updates.
  Crop bounds, alpha classification, output formats, and the
  RGB565 path remain compatible. Added full rim-boundary/randomized comparisons and reproducible
  host timing. This optimizes image planning on initial loads/cache misses; device loading time
  and whole-system frame-rate gains remain unmeasured.

## Theme pages reuse applied looks

- Skip target look assembly/comparison when the active rule set and alarm suppression are
  unchanged. Conditions, timing, and bindings still update; rule exits, image/video switches,
  suppression changes, and page background recovery retain their behavior. Added real-LVGL
  host replay and stable-page desktop benchmarks; CI enables page comparisons. Device gains
  remain unmeasured.

## Vehicle and video validation reuse ROM CRC

- Compiled vehicle profiles and OBV1 header/index CRC32 checks use the existing ESP32 ROM
  API, reducing computation and private lookup data during import and video opening.
  Standard CRC, empty-input, and incremental semantics remain compatible, and portable
  host implementations remain available. Add SDK compatibility comparisons and a reproducible
  desktop benchmark; device timings remain unmeasured.

## Theme rule output cache

- Conditions, hysteresis, debounce and trigger state still advance on every evaluation.
  An unchanged active set reuses sorted actions and target coverage, reducing repeated theme
  work. Set changes or resets rebuild output, preserving rule exits, override priorities and
  shared ID behavior. Add comparison tests, reproducible host benchmarks and bilingual docs;
  whole-device gains still require measurement.

## Runtime performance improvements and device statistics

- CAN/RPN field extraction consumes byte fragments and removes the data-dependent sign branch.
  RaceChrono selects subscribed, due channels before batched reads, skips idle snapshots, and
  uses synchronized channel-indexed filters. Sending intervals, invalid-data handling, and
  retired PIDs remain compatible. Added default-off `CONFIG_OBD_PERF_STATS` for UI/theme/OBD/SD/
  video/RaceChrono duration distributions and heap headroom, plus bilingual audit and reproducible
  benchmarks. Host timings and simulated workload results are available; device frame rate,
  wireless/SD throughput, and stability have not been measured.

## Vehicle profiles inherit OBD2 by default

- Vehicle JSON automatically inherits 9 omitted standard signals from built-in OBD2;
  `bus` / `signals` are optional. Explicit signals replace their whole default;
  `disabled: true` suppresses one signal and root `defaults: "none"` disables inheritance.
  Gear/drivetrain still require explicit definitions. Validation and serial polling
  use the completed profile. Added minimal and BMW G sparse gear examples; existing
  complete catalog profiles and the import pack remain compatible.

## Editable vehicle import pack

- Added 18 complete `custom.*` JSON templates in `vehicles/import/OBD/IMPORT/`:
  copies of the 17 catalog profiles plus a BMW G direct gear template. Request
  headers, response prefixes and minimum polling intervals are explicit and editable.
  BMW G reuses the F/G protocol 6 DID `DA2E` path with ratio fallback; the original
  protocol 7 variant stays separate. Uses existing SD / API import flows, with
  generator and firmware-validator checks. No additional vehicle testing.

## Vehicle system, SD storage and theme engine overhaul

The partition table is upgraded to layout v2 (layout_id `0x94752aa3`).
**Upgrading from a previous release requires one full reflash using the web
flasher's complete mode**, after which single-firmware OTA works as usual.
**Do not** use `esptool.py erase_flash` — it also erases user settings and
Bluetooth pairings stored in NVS.

- **Vehicles are now JSON profiles**: the firmware only builds in one
  vehicle, `Generic OBD2`; the other 17 vehicles (every vehicle that used to
  be built in) became `*.vehicle.json` files under `vehicles/catalog/`,
  installed on demand from the SD card / the app / the gauge's vehicle page.
  Lifecycle is INSTALLED → PENDING → ACTIVE; a failed 20-second verification
  rolls back to the previous vehicle, repeated failures roll back to Generic
  OBD2. After the one required full web reflash, the firmware automatically
  migrates the old vehicle selection to the matching vehicle JSON (restored
  automatically when found under `/OBD/CATALOG/`, otherwise Generic OBD2
  runs in the meantime)
- **New SD card storage**: import/export vehicles and themes, an SD theme
  library (the Settings "Storage" page can switch themes, the "Vehicle" page
  can switch/install vehicles), and boot animations can now live on the SD
  card too
- **New boot video format, OBV1** (`.obv`, MJPEG container, produced by
  `tools/make_video.py`), while staying compatible with the legacy flash
  bootmedia format
- **ESP-NOW upgraded to v6** (TLV encoding); the master keeps sending v5
  compatibly until every slave has upgraded, so a mixed-firmware triple-gauge
  setup never disconnects
- **App OTA protocol unchanged**: the `/ota/*` endpoints are untouched; new
  `/api/v1/*` endpoints (vehicle install/activate, called by the companion
  app) and the gauge's own web page (OTA mode only; theme sidecar / file /
  log maintenance tools, no vehicle-management UI)
- **New crash dump storage** (coredump partition, ELF+CRC32); `GET
  /ota/coredump` returns 204 when no dump is stored
- **The brake-temperature (RS485) module and its BLE OTA service were
  removed entirely**

## Documentation system rebuild (bilingual)

Everything this branch changes relative to main, summarized as one
documentation rebuild — no firmware behavior changes:

- **Restructure**: the root README is just the entry point (identity, quick
  start, index, 3D models, license); the details moved into seven
  audience-scoped guides — `USER_GUIDE` (usage), `FLASH` (flashing and
  updates, user-facing), `APP_PROTOCOL` (app integration protocol,
  developer-facing), `VEHICLES` (vehicle profiles), `THEMES` (themes,
  merged from four old theme docs), `TROUBLESHOOTING` (merged from three
  old docs) and `DEVELOPMENT` (development guide, incl. the release
  workflow); the `firmware/` / `themes/` / `theme_store/` directory READMEs
  became slim pointers. Twelve obsolete documents were removed, including
  `docs/BRANCH_COMPARISON.md` describing a two-branch world that no longer
  exists
- **Fixed dangerous stale facts**: some old tables still flashed bootmedia
  to `0x620000` — that address is now the theme_0 partition, so following
  them destroyed the theme partition. Unified to bootmedia `0xA20000` /
  theme `0x620000`, governed by `partitions.csv`. Also corrected the
  vehicle profile count (12 → **17**), the `model/Subaru/brz_zc6/` path,
  the "app partition is 4 MB" claim (it is 3 MB) and similar
- **Bilingual**: all 11 documents now have 1:1 mirrored English editions
  (`X.md` ↔ `X.en.md`) with a language-switch line under the title;
  English editions cross-link via `.en.md`. Chinese remains the source of
  truth; the maintenance rules live in `DEVELOPMENT.md`
- **Documentation-in-code fixes**: `partitions.csv` bootmedia size comment
  7.875 MB → 5.875 MB; `ads1115_oil_pressure.h` header corrected from
  "direct ESP32 ADC" to the ADS1115 I2C ADC; `make_boot_block.py` docstring
  gained the _v2 format; `one_shot.py` / `analyze_proble.py` no longer
  reference the nonexistent `analyze_probe.py` filename

## Theme-engine fixes and direct BMW gear read

- BMW F/G reads the current gear directly via the EGS extended-address DID
  `DA2E` (request header `ATSH6F1`, receive filter `ATCRA618`), replacing
  ratio estimation
- Intake air temperature (IAT) added to the theme data snapshot; fixed the
  "--" tofu on the OBD gear page when no data was available
- Theme engine rolled back to build 118 (regressions from the conditional
  rules engine)
- Fixed CST816 touch I2C pins; touch now runs on its own bus

## Linked RPM warning sync across the cluster

- The master now broadcasts `rpm_warn_linked_en` to slaves, so the three
  gauges no longer need per-gauge setup
- In linked mode the gauges light up in position order and all flash at the
  threshold

## New vehicle profiles (17 built in)

- Added `Supra A90` (B58, OBD oil-pressure DID `4436` replacing ADS1115),
  `BMW E` (N55 oil temp `4402`/`5822` and oil pressure `586F` over the
  `6F1` header), `MINI R55`, `jeep`, and `Honda Integra` (29-bit functional
  addressing `18DB33F1`, CVT with ratio estimation disabled)
- Full capability table in `docs/VEHICLES.md`

## V2/V3 hardware variants (tai_ji_xiao_pai 24/25)

- Build-time hardware-version choice added (menuconfig → OBD DSP
  Configuration): V1 Waveshare board (default) / V2 new board A (ST77916 v1
  init sequence) / V3 new board B (v2 sequence); V2/V3 are direct-GPIO
  boards without TCA9554 / ADS1115

## Theme partition system (theme_0)

- New 4 MB `theme_0` partition (`0x620000`) holding runtime themes:
  manifest + layout.json + 360×360 dial/ring artwork, loaded at boot by
  `theme_engine`, with automatic fallback to the built-in default theme on
  corruption
- **bootmedia moved `0x620000` → `0xA20000`, shrunk 9.875 MB → 5.875 MB**
  (actual usage ~312 KB)
- WiFi OTA gained `/ota/theme`, `/ota/theme/prepare` and `/ota/theme/erase`;
  BLE-side theme transfer is not implemented yet
- Theme-declared pages replace the built-in gauge pages in the carousel
  (saving 40–60 KB RAM); `logo` / `intro` / `boot_video` are protected and
  never themed
- ⚠️ devices on the old partition table must be upgraded with a **one-time
  full USB reflash** — OTA cannot rewrite the partition table

## On-device OTA: BLE service + WiFi transfer

- New **OTA mode screen** (entered via the **OTA button on the version
  page**): publishes the OTA BLE service (`0x1FFB`) and starts a WiFi
  SoftAP (`OBD-Gauge-OTA-XXXX`, password `obd2024`) whose HTTP endpoints
  accept SHA256-verified firmware and boot animations
- Firmware lands in the inactive OTA slot and is marked valid only after a
  15 s boot self-check; early crashes roll back via the bootloader
- Boot-animation updates are transactional (`boot_block.txt.new`/`bin.new`
  staged, then atomically committed); RS485 and ESP-NOW pause during
  transfers
- The version page shows the firmware build tag (branch-commitcount-shorthash,
  injected at build time)

## RaceChrono toggle and simplified boot-animation modes

- New **RACECHRONO** toggle in Settings: OFF = minimal BLE mode (Info + OTA
  services only, no advertising); ON = full RaceChrono + pairing + OTA set
- Boot-animation modes simplified to **OFF / RACE / VIDEO**; VIDEO plays a
  custom animation flashed from the phone app (replacing the old
  REI/SHINJI/ASUKA slots, with legacy values migrated to VIDEO)

## OBD data-path fixes

- The ELM327 client runs **single-threaded**; no more interleaved OBD/CAN
  parallel requests
- Brake-temperature / oil-pressure alarms throttled to one alert per 30 s
- Self-healing data link: re-init + auto-reconnect on stalls — power on and
  drive, no manual reconnect

## OTA dual-slot layout and device manifest

- Partition table moved to `ota_0` + `ota_1` (3 MB each) + `bootmedia` with
  rollback support
- A read-only BLE manifest service (`0x1FFA`) exposes hardware and build
  info for the app's compatibility check; the manifest carries only fields
  the app reads, staying under 512 bytes

## Themes as data (compiled TOML themes)

- Themes moved from C code to **data declarations**: `theme.toml` + artwork
  PNGs under `themes/`, compiled into `ui_theme_generated.c` by
  `tools/gen_themes.py` at CMake configure time
- 8 decorative color roles + 3 optional artwork kinds (bezel/needle/dial)
  under a 1536 KB budget
- `registry.txt` slots are append-only, preventing silent re-skins after OTA
- Semantic colors (warning red etc.) are global and never themed
