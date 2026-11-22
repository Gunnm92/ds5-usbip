# libvirtualhid facts for the DS5 USB/IP backend

Pinned commit: `dd8153df338c6fb399ea4a048fa97b4c536f12a0` (Sunshine `LIBVIRTUALHID_COMMIT`).
Line numbers refer to the pinned commit, not to the patched tree (patch 0001 changes the line numbers of `uhid_backend.cpp` from line 3142 onward).
Checkout: `/config/workspace/libvirtualhid`, branch `ds5-usbip` (pinned commit plus local patch commit `ea4c8cd`).

## 1. Exceptions and `-fno-exceptions`

- **Does not compile with `-fno-exceptions`.** `src/core/runtime.cpp:58-60` uses `try { callback_(level, message); } catch (...)`. Checked locally with `g++ -std=c++23 -fno-exceptions -fsyntax-only -Isrc/include -Isrc src/core/runtime.cpp`: `error: exception handling disabled, use '-fexceptions' to enable` at `runtime.cpp:60`.
- Exceptions used in `src/`: only that one `try/catch(...)` (`runtime.cpp:58-60`). No `throw` in `src/`. The library is written with `noexcept` (87 occurrences in `src/`) and status objects (`OperationStatus`), so exceptions are a small, localized dependency.
- Tests use exceptions freely (`tests/unit/test_linux_consumers.cpp:107-111`, `tests/unit/test_runtime.cpp:88`).
- Build flags do not disable exceptions (`CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt` contain no `-fno-exceptions`).

## 2. Test hook mechanism

- Macro redefinitions: `tests/fixtures/linux_backend_test_hooks.cpp:557-576` defines, for the Linux backend, `access`, `ioctl`, `open`, `poll`, `read`, `write` (lines 558-563), plus libevdev functions (`libevdev_enable_event_code`, `libevdev_enable_event_type`, `libevdev_enable_property`, `libevdev_free`, `libevdev_new`, `libevdev_set_id_*`, `libevdev_set_name`, `libevdev_uinput_create_from_device`, `libevdev_uinput_destroy`, `libevdev_uinput_get_devnode`; lines 564-576). When `LIBVIRTUALHID_HAVE_XTEST` is set, X11 symbols are also redefined (lines 578-601: `XOpenDisplay`, `XTestFakeKeyEvent`, etc.).
- `uhid_backend.cpp` is included at `linux_backend_test_hooks.cpp:603-604` (`#define create_platform_backend create_platform_backend_for_linux_backend_test_hooks` followed by `#include "../../src/platform/linux/uhid_backend.cpp"`), inside the `#if defined(__linux__)` block that starts at line 65. The include happens at file scope, so the `lvh::detail` namespace and the anonymous namespace of `uhid_backend.cpp` become visible to the wrappers. Macros are undone at lines 606-635 (`#undef write`, `#undef read`, ...).
- Wrapper functions live in `namespace lvh::detail::test` (`linux_backend_test_hooks.cpp:642` onward). Each one is a free function that calls backend internals directly, e.g. `linux_dualsense_mac_address` (defined at `:1087`) and `linux_dualsense_uhid_socketpair_reports` (defined at `:2036`, builds a `UhidGamepad` on a `socketpair` and exchanges UHID events).
- Adding a wrapper: (a) declare it with a Doxygen comment in `tests/fixtures/include/fixtures/linux_backend_test_hooks.hpp` inside `namespace lvh::detail::test` (existing examples at `:728` and `:1115`); (b) define it in `tests/fixtures/linux_backend_test_hooks.cpp` inside the `namespace lvh::detail::test` block, after the `uhid_backend.cpp` include, so it can call internals (examples at `:1087`, `:2036`, `:2130`). Any new `.cpp` that calls internals must also be included in the same TU, so put new internal-facing code in that fixture or in a new fixture included the same way.
- Registering tests: `tests/unit/test_linux_backend.cpp` includes `fixtures/linux_backend_test_hooks.hpp` at line 31 and defines tests with `TEST_F(LinuxBackendTest, Name)` (`class LinuxBackendTest: public LinuxTest {}` at line 36). DualSense feature-report test: `TEST_F(LinuxBackendTest, SocketpairBackedDualSenseRepliesToFeatureReports)` at `:1101`; Bluetooth framing test at `:1118`.
- New test source files: add to `LIBVIRTUALHID_TEST_SOURCES` in the Linux branch of `tests/CMakeLists.txt:66-70` (fixture at line 68, `test_linux_backend.cpp` at line 69, `test_linux_consumers.cpp` at line 70). Test discovery is `gtest_discover_tests(${TEST_BINARY})` at `tests/CMakeLists.txt:208`.

## 3. DualSense USB input report: time-dependent fields and periodic repacking

- Time-dependent, yes, in both USB and Bluetooth framing (`pack_dualsense_input_report`, `src/core/report.cpp:755`):
  - Sequence counter: `dualsense_sequence_number()` (`report.cpp:650-653`), a process-wide `static std::atomic_uint32_t` that increments on each call and wraps with `% 255`. Written at `report.cpp:775` (`payload_offset + 6`).
  - Sensor timestamp: `dualsense_sensor_timestamp()` (`report.cpp:655-660`), derived from `steady_clock` in units of 1/3 microsecond. Written at `report.cpp:836` (`payload_offset + 27`, u32).
- Also time-dependent in DualShock 4 (`dualshock4_sensor_timestamp`, `report.cpp:643-649`, written at `:734`).
- Periodic repacking: `UhidGamepad::periodic_report_loop` (`uhid_backend.cpp:3451-3469`) sleeps `playstation_periodic_report_ms` (10 ms, `uhid_backend.cpp:117`) and calls `reports::pack_input_report(profile_, last_state_)` at `:3464`, then `write_input_report`. It runs for every PlayStation profile and for Switch Pro (`uhid_backend.cpp:3226-3230` start condition; the 15 ms Switch Pro interval is at `:118`).
- Consequence for USB/IP: a DualSense USB/IP backend that bypasses `pack_input_report` must reproduce the sequence and timestamp semantics itself if the host checks them. Identical bytes are not possible across runs (timestamp), so any byte-level test must mask them: USB report bytes 7 (sequence) and 28..31 (timestamp); Bluetooth report bytes 8 and 29..32 (`payload_offset` is 1 for USB, 2 for Bluetooth).

## 4. `create_gamepad` and where to insert the USB/IP attempt

- Linux entry point: `LinuxUhidBackend::create_gamepad(DeviceId id, const CreateGamepadOptions &options)` at `src/platform/linux/uhid_backend.cpp:3670`. The `#if defined(__linux__)` block starts at `:3671`, and the first branch is `if (prefers_uhid_gamepad_profile(options.profile.gamepad_kind))` at `:3672`.
- Insert the USB/IP attempt between `:3671` (`#if defined(__linux__)`) and `:3672`, before the UHID branch. That keeps it ahead of `/dev/uhid` and `/dev/uinput` fallback. On failure it must fall through (not return) unless the profile is USB/IP-only, mirroring the UHID branch's use of `uses_uinput_gamepad_profile`.
- Detect the profile: `options.profile.gamepad_kind == GamepadProfileKind::dualsense && options.profile.bus_type == BusType::usb`. Enum: `src/include/libvirtualhid/types.hpp:244-251` (`BusType::usb`), `:253-262` (`GamepadProfileKind::dualsense`). The USB DualSense profile is `profiles::dualsense_usb()` (`src/core/profiles.cpp:2123-2125`, `make_dualsense_profile(BusType::usb)`), declared in `src/include/libvirtualhid/profiles.hpp:91`.
- `prefers_uhid_gamepad_profile` is at `uhid_backend.cpp:292`; `is_playstation_profile` at `:260-262`.

## 5. CMake sources, language standard, flags

- Library source list: `src/CMakeLists.txt`, function `libvirtualhid_add_library`. Core sources at lines 32-38. Linux sources are in the `if(LIBVIRTUALHID_USES_THREADS)` branch (`src/CMakeLists.txt:41-70`, with the Linux entries at lines 67-70) (`platform/linux/uhid_backend.cpp` at line 69, `platform/linux/output_mapping.cpp` at line 70). Add new `usbip/*.cpp` there, e.g. `"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/platform/linux/usbip/<file>.cpp"`. The same list is reused for the test library (`libvirtualhid_add_library(libvirtualhid_test)` at `tests/CMakeLists.txt:33`).
- Standard: `CMakeLists.txt:14` `set(CMAKE_CXX_STANDARD 23)`, `:15` `REQUIRED ON`, `:16` `EXTENSIONS OFF`. Per target: `src/CMakeLists.txt:3` `target_compile_features(... PUBLIC cxx_std_23)`.
- Flags (`src/CMakeLists.txt:21-23`): `-Wall -Wextra -Wpedantic`; with `LIBVIRTUALHID_WARNINGS_AS_ERRORS`, `-Werror`. Test-only: `-fprofile-arcs -ftest-coverage -ggdb -O0` (non-MSVC, `tests/CMakeLists.txt:29-30`).
- The include path for `shared/...` and `platform/...` is the library's own source dir (`src/CMakeLists.txt`, `target_include_directories(... PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")`), so `#include "shared/playstation_feature_reports.hpp"` resolves.

## 6. Cache options and CPM

- Tests: `option(BUILD_TESTS "Build tests" ...)` at `CMakeLists.txt:37`. Warnings: `option(LIBVIRTUALHID_WARNINGS_AS_ERRORS ...)` at `CMakeLists.txt:55`. Also `BUILD_DOCS` (`:35`), `BUILD_EXAMPLES` (`:36`), `LIBVIRTUALHID_BUILD_TOOLS` (`:38`) are top-level defaults.
- Linux test dependencies (tested in Docker, see `Dockerfile.test`): libevdev, libinput, sdl2 (`tests/CMakeLists.txt:58-60`, `pkg_check_modules REQUIRED`), SDL3 built by CPM from `package-lock.cmake` (SDL3 3.4.12, static), GoogleTest and lizardbyte-common from `third-party/` submodules (run `git submodule update --init --recursive`). The CPM-built SDL3 configure stops on missing X11 dev packages (first failure: XRANDR), so the image installs `libxrandr`, `libxcursor`, `libxi`, `libxss`, `libxext`, `libxkbcommon`, `wayland`, `wayland-protocols`, `vulkan-headers` in addition to the base set.
- CPM package name in Sunshine: `libvirtualhid` (Sunshine's package lock variable `LIBVIRTUALHID_COMMIT`). The libvirtualhid tree itself does not declare a CPM package for itself, so this name is only visible from Sunshine. Standard CPM behaviour is that `CPM_<NAME>_SOURCE` overrides the source location, so `-DCPM_libvirtualhid_SOURCE=/path/to/libvirtualhid` should work; I have not verified this against a Sunshine checkout (none in this environment).

## 7. Differences between pinned commit and master

- `git fetch origin`: `origin/master` is `82270d4`, 3 commits ahead of `dd8153d`.
- `src/platform/linux/uhid_backend.cpp` is byte-identical between `dd8153d` and `origin/master` (`git diff --quiet` exits 0). The 3 commits touch other files (5 insertions, 26 deletions in total, none in the Linux UHID backend).
- So the pinned commit and master are equivalent for `UhidGamepad`; a patch made against `dd8153d` applies to master as well, but rebase is still needed when Sunshine moves its pin.

## 8. Docker test results (for the record)

- Baseline (pinned, no patch) and patched trees configure with `-DBUILD_TESTS=ON -DLIBVIRTUALHID_WARNINGS_AS_ERRORS=ON` and build.
- ctest: 175 tests pass. 21 tests are excluded because they need `/dev/uhid` or `/dev/uinput` on the host (the `LinuxRuntimeTest.Linux{Uhid,Uinput}SmokeTestRequiresPrerequisites` tests and 19 `LinuxConsumerTest` tests). These fail in the BuildKit container because it has no such devices; see `LVH_DEVICE_TESTS` in `Dockerfile.test`.

## 9. UsbipGamepad (patch 0002)

Selection and fallback:
- Used only for `GamepadProfileKind::dualsense` with `BusType::usb`, when `LIBVIRTUALHID_DUALSENSE_USBIP=1`. It is tried first in `LinuxUhidBackend::create_gamepad`. On failure one line goes to stderr, `libvirtualhid: DualSense USB/IP unavailable (<reason>), using uhid`, and the uhid path runs unchanged. Reasons: `vhci-hcd not loaded or attach not writable`, `socketpair failed: ...`, `failed to start the USB/IP session`, `vhci-hcd status is unreadable`, `no free high-speed vhci port`, `attach write failed: ...` (any errno but EBUSY), `vhci-hcd ports stayed busy` (three EBUSY attaches), `vhci-hcd closed the connection before enumeration`, `enumeration timeout` (no SET_CONFIGURATION within 2 s).

Environment variables:
- `LIBVIRTUALHID_DUALSENSE_USBIP=1`: enables the backend.
- `LIBVIRTUALHID_VHCI_SYSFS`: vhci-hcd sysfs directory (default `/sys/devices/platform/vhci_hcd.0`). Needs `status`, `attach`, `detach`.
- `LIBVIRTUALHID_DUALSENSE_HAPTICS_RTP=IP:PORT`: sends haptic channels 3-4 as RTP (library `HapticsRtpSender`). An invalid destination disables haptics with a stderr line. Used only when `GamepadMetadata::haptics_destination` is empty (see below).
- `LIBVIRTUALHID_DUALSENSE_HAPTICS_GAIN=<float>` (default 1) and `LIBVIRTUALHID_DUALSENSE_HAPTICS_GAIN_FILE=<path>` (re-read every second by the library).
- `LIBVIRTUALHID_DUALSENSE_RTP_CHANNELS=4`: sends 4 channels (speakers 1-2 + haptics 3-4) instead of haptics only.
- `LIBVIRTUALHID_DUALSENSE_SETTINGS=<path>` (unset = none): settings file, see below.
- `LIBVIRTUALHID_DUALSENSE_STATUS=<path>` (unset = none): status file, see below.

Haptics destination (patch 0003): `CreateGamepadOptions::metadata.haptics_destination` (`GamepadMetadata::haptics_destination`, set by the host application, e.g. the streaming client's address) wins when non-empty. Otherwise `LIBVIRTUALHID_DUALSENSE_HAPTICS_RTP` is used. Otherwise no RTP is sent.

Settings file (`LIBVIRTUALHID_DUALSENSE_SETTINGS`): text, one `key=value` per line, `#` starts a comment.
- `gain`: float, `0 < g <= 64`. Read at pad creation and overrides `LIBVIRTUALHID_DUALSENSE_HAPTICS_GAIN`.
- `channels`: `2` or `4`. Read at pad creation and overrides `LIBVIRTUALHID_DUALSENSE_RTP_CHANNELS`.
- Unknown keys and invalid values are ignored; a missing or unreadable file is ignored silently.
- Without `LIBVIRTUALHID_DUALSENSE_HAPTICS_GAIN_FILE`, the settings file is the gain file: it is re-read every second while the pad lives, so a `gain=<float>` line changes the gain on the fly. The gain file also accepts a plain number (the older format).
- Parsing and the gain reload are in the library: `ds5usbip::parse_haptics_settings`, `load_haptics_settings`, `parse_gain_text` (`src/usbip/haptics_rtp.hpp`).

Status file (`LIBVIRTUALHID_DUALSENSE_STATUS`): while a USB/IP DualSense is attached, rewritten about once per second (first write right after enumeration) through `<path>.tmp` and a rename. One JSON object, one line:

```
{"attached":true,"vhci_port":1,"destination":"127.0.0.1:46100","channels":4,"gain":2.5,"peak_db":[-12.3,-40.1,-9.8,-7.2],"packets":1234,"updated_ms":1768000000000}
```

- `vhci_port`: port named in the attach line. `destination`: the RTP destination in use, empty when none. `channels`: 2 or 4. `gain`: current haptics gain (after any reload). `peak_db`: per channel 1-4, peak over the last second in dBFS, `-120.0` for silence. Peaks are taken from the PCM given to the audio callback, before gain. `packets`: RTP packets sent. `updated_ms`: Unix epoch in milliseconds.
- On `close()`, one closing record is written: `{"attached":false,"updated_ms":...}`. Write errors are ignored.
- The status writer is a dedicated thread (not the session threads or the dispatcher). Only one pad at a time is expected: with several pads, the last writer wins.

Attach and teardown:
- Socketpair: `pair[0]` goes to `UsbipSession`, `pair[1]` is written as `port fd 0x10001 3` to `attach`, then closed (vhci-hcd keeps its own reference). Ports come from `status` (first free `hs` port, skipping busy ones).
- Enumeration is complete when the host sends SET_CONFIGURATION (`DualSenseDevice::configured()`), polled every 10 ms.
- `close()`: writes the port to `detach` (best effort), stops the session (shutdown + close, so the peer reads EOF), joins the dispatcher, drops queued reports and destroys the haptics sender. Idempotent; the destructor calls it.

Threads:
- Caller thread: `create`, `submit`, `device_nodes`, `close`.
- Library session threads: reader (control, interrupt OUT, SET_REPORT, audio callback that feeds haptics) and timer (HID IN every 4 ms, which packs the report from the last submitted state with a fresh sequence number and timestamp).
- Dispatcher (`std::jthread`): the only thread that calls the `OutputCallback`. Output and SET_REPORT(feature) bytes are queued in a 256-entry FIFO; when full, the oldest is dropped and the drop count is logged at close.

Feature reports: GET_REPORT(feature) is answered by `playstation_feature_report` (same table as uhid, MAC from `stable_id`). SET_REPORT(feature) data is queued like output; it already starts with the report id, so this matches uhid's `dispatch_set_report` for the same bytes.

Known limitations:
- Only the USB DualSense profile. No Bluetooth, no hotplug, no re-attach after vhci-hcd drops the port.
- Detach is best effort and the port is not polled until free; the session close is what guarantees unplug.
- `device_nodes()` scans the first high-speed `usbN` (speed 480) for `N-<port+1>:1.3/0003:*`. Only the test fake tree covers it, not a real `/sys` layout.
- Tests use a fake sysfs and a socketpair; nothing has been attached to a real vhci-hcd or a real host here.
- The dispatcher-thread check asserts only that the callback is not on the caller thread. Library session threads are private, so their IDs cannot be compared.
- Library defect: `UsbipSession` (`src/usbip/usbip_session.cpp`) reads `hid_in_count_` in `reader_loop` (for the final log) while `timer_loop` increments it without synchronisation. This is a data race; see the TSan notes in the report.
- Library difference: GET_REPORT(feature) with an unknown id returns a zero report, while uhid returns EINVAL.
- Status file and settings: covered by fixture tests (fake sysfs, socketpair, temp dir) that check the exact record layout and the destination, channels and gain fields. The `peak_db` values are not checked by an automated test (the fixture does not drive the isochronous audio path), and the gain hot reload is tested only in the library (`tests/cpp/session_test.cpp`).
