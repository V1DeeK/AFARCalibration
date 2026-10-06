## Spectrum Analyzer

Built on an AD9361-based SDR (PlutoSDR / Zynq+AD9361 eval boards, e.g. ZC702+FMCOMMS2/3) connected over `libiio`, streaming coherent dual-channel IQ through a lock-free producer/consumer pipeline in C++17, with Hann windowing, FFTW3 transforms, and Welch averaging for a stable noise floor. Live PSD and scrolling waterfall rendered via Dear ImGui and ImPlot, single- or dual-channel side by side. Tunable 70 MHz – 6 GHz (operation observed from ~50 MHz), 1–61 MSPS. What started as a PSD/waterfall tool has grown into a small multi-mode receiver on the same coherent capture pipeline: wideband monopulse direction finding, FM audio demodulation with live playback, and analog TV (NTSC/PAL, with color) video demodulation. Recorded IQ can be replayed through the exact same pipeline for offline testing without hardware attached. Includes Python scripts for offline IQ analysis, V2X band interference characterization, and synthesizing test IQ files.

---

## What It Does

**Capture & connection**
- Connects to any AD9361-based `libiio` context over the network (`ip:...`) or locally (`local:`, for running directly on a Zynq+AD9361 board) — scan for devices on the LAN or enter an IP/URI manually
- Streams coherent IQ from RX1, RX2, or both simultaneously, at a configurable sample rate/bandwidth (1–61 MSPS) and RF port (A/B/C balanced or single-ended)
- Scales samples to normalized complex floats and pushes into per-consumer lock-free circular ring buffers (PSD, direction finding, and demod each get their own)

**PSD & waterfall**
- Processing thread pulls batches, applies a Hann window, runs an FFTW3 forward transform, and accumulates magnitude-squared output using Welch's method across 8 frames
- Converts averaged power to dBm via `10 * log10(|X[k]|² / N²) + cal_offset`, applies fftshift to center DC, and interpolates the DC bin to suppress the hardware offset artifact
- Live PSD line graph and scrolling waterfall heatmap, rendered as single or side-by-side dual-channel panes when both RX channels are active
- Controls: center frequency (0–6000 MHz), FFT size (1024 up to 65536), calibration offset, peak hold, cursor dBm readout, Save IQ

**Signal quality: clipping and SNR**
- Every sample that reaches full scale on the ADC is counted, on live capture and on file playback alike, and the strip under the waterfall marks *when* it happened: red lines are the clipping moments, the green trace is the per-window peak level in dBFS against the full-scale line. Hovering a window gives the exact share of clipped samples in it
- The counting is done on the raw int16 codes, before the /2048 normalization and before the Rx2 calibration rotation — after those a rail-hitting sample is no longer at the rail, and clipping becomes indistinguishable from a merely loud signal. The threshold sits slightly below full scale (code 2000, −0.2 dBFS) because what leaves the AD9361 is the output of its decimators and FIR, where a clipped peak rarely lands exactly on code 2047
- The status bar shows `CLIP x.xx%` while it is happening and keeps `clipping seen (N windows, worst x.xx%)` afterwards, so a burst that lasted one second in a ten-minute session is still visible. **Reset clip stats** clears the counters
- Save IQ scans what it writes with the same threshold and reports the verdict next to the button — clipping is worth knowing about while the setup is still assembled and the gain can still be lowered; nothing recovers it from the finished file
- SNR is published with every spectrum: the noise floor is the median over FFT bins (the median holds onto the noise as long as signals occupy less than half the band, where the mean would be dragged up by any strong carrier), the signal is the strongest bin. This is peak-over-noise **in one bin**, the number a spectrum analyzer shows — a demodulator sees less, since its noise is collected over the whole channel bandwidth. The numbers are in the Display section; **mark on plot** draws the noise floor line and the SNR label at the peak, and the PSD cursor tooltip reads out how far the cursor sits above the same floor

**FM audio demodulation**
- Decimates and FM-discriminates the selected channel, applies de-emphasis and low-pass filtering, resamples to the audio device's native rate, and plays it live through the system's default output via `miniaudio`
- Waveform panel shows a rolling snapshot of the demodulated audio

**Analog TV video demodulation**
- Same decimate → FM-discriminate front end, then a sync-detection and line/frame assembly state machine decodes NTSC or PAL composite video, including NTSC color-burst-locked chroma decode (PAL is luma-only)
- Rendered live as an RGB frame in a detachable window (OpenGL texture); a lock indicator shows whether NTSC color-burst phase lock has been acquired
- Optional audio subcarrier demod for FPV/analog-TV-style combined video+audio signals

**IQ playback**
- Replays a recorded `.bin` file (from Save IQ, `python/psd_generate.py`, or `python/mp4_to_iq.py`) through the same PSD/direction-finding/demod pipeline live capture feeds — so every mode above works unmodified on recorded data, for testing without hardware
- Single- and dual-channel recordings both replay. A dual-channel file holds `i1,q1,i2,q2` per sample, so the layout has to be known up front — a file gives no hint of it and parsing one as the other reads Rx2's samples as Rx1's *next* samples, which shifts the whole spectrum rather than merely dropping a channel. Save IQ marks dual-channel recordings with a `_2ch` suffix and the **2 ch** checkbox next to the playback path follows it; names without the suffix replay as single-channel, as every recording made before it existed does
- Direction finding on a recording needs a dual-channel file — monopulse has nothing to work with from one channel (Σ = Rx1, Δ = −Rx1, so ρ is a constant −1 and every subband masks out)
- Includes a raw I/Q scope pane for visually inspecting what's being replayed, independent of the demodulated-audio waveform panel

---

## Hardware

| Component | Detail |
|-----------|--------|
| SDR | Any AD9361-based board reachable via `libiio` (PlutoSDR / Zynq+AD9361 eval boards, e.g. ZC702+FMCOMMS2/3) |
| RFIC | Analog Devices AD9361 |
| Frequency range | ~70 MHz – 6 GHz (operation observed from ~50 MHz) |
| Sample rate | 1–36 MSPS, configurable (AD9361 limit) |
| RX channels | RX1, RX2, or both coherently (required for direction finding) |
| Interface | Network (`ip:`) or local IIO context (`local:`) via libiio |

---

## Dependencies

```bash
# System
sudo apt install libiio-dev libad9361-dev libfftw3-dev libglfw3-dev libgl1-mesa-dev

# ImGui and ImPlot (source build)
git clone https://github.com/ocornut/imgui third_party/imgui
git clone https://github.com/epezent/implot third_party/implot

# miniaudio (single header, used for live FM/TV audio playback)
mkdir -p third_party/miniaudio
curl -L -o third_party/miniaudio/miniaudio.h \
  https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h

# Only needed to build the board-side program (see "Board-Side Statistics Link")
sudo apt-get install g++-arm-linux-gnueabihf

# Python
sudo apt install python3-numpy python3-matplotlib

# Only needed for python/mp4_to_iq.py (encodes a video into a synthetic
# analog-TV IQ recording, for testing TV video demod without RF hardware)
pip3 install --user opencv-python-headless
# ffmpeg (for the audio subcarrier) — system package, or a no-sudo fallback:
sudo apt install ffmpeg   # OR: pip3 install --user imageio-ffmpeg
```

---

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Debug build with AddressSanitizer:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address
```

Debug build with ThreadSanitizer:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
```

---

## Windows Build (cross-compiled from Linux)

`spectrum_analyzer.exe` is cross-compiled with mingw-w64, using the same
libiio + libad9361 network-transport backend as the native Linux build.
FFTW3, GLFW, libxml2, libiio, and libad9361 are all built from source into
`build-win/deps-prefix` and linked statically, so the result is a single
portable `.exe` — no DLLs to ship alongside it.

Install the toolchain once:
```bash
sudo apt-get install g++-mingw-w64-x86-64 gcc-mingw-w64-x86-64 mingw-w64-tools cmake ninja-build
```

Then build:
```bash
./scripts/build-windows.sh
```

Output: `build-win/spectrum_analyzer.exe`. The script is safe to re-run — each
dependency is skipped once its install marker exists in `build-win/deps-prefix`;
delete `build-win/` for a clean rebuild.

**Why `lib/libiio` and `lib/libad9361-iio` are checked out at old tags
(v0.25 / v0.3):** the app code (and Ubuntu's `libiio-dev`/`libad9361-dev`
packages, which the native Linux build above actually links against) use the
pre-1.0 libiio API. Upstream's default branch is the newer 1.x API, which
changes/removes several functions this app calls — building that version
fails against this source tree.

---

## Run

```bash
./spectrum_analyzer
```

The display opens immediately, disconnected. In "Hardware Connection Settings":
- **Scan Devices** discovers AD9361/Pluto `libiio` contexts on the local network, or enter an IP/URI manually (e.g. `ip:192.168.2.1`, or `local:` when running directly on the SDR's own Zynq/PetaLinux host)
- Pick **Channels** (Rx 1 / Rx 2 / Rx 1 + Rx 2 — dual-channel is required for direction finding) and **RF Port**, set sample rate/bandwidth, then **Connect to Pluto**

Once connected the capture thread streams continuously; changing frequency, FFT size, channels, or RF port takes effect live without reconnecting (channel/port/bandwidth changes do trigger a reconnect).

**Finding the board when its IP has moved.** DHCP hands the board a new address between boots, so the Device panel has a **Board MAC** field with a **Find board** button. It searches the local /24 for that MAC and, on success, offers **Use as device** to fill in `ip:<address>`. The MAC is what stays constant, which is why the search keys on it rather than on a remembered IP.

### Build the board program

The board is a 32-bit ARM Cortex-A9, so `fzstats_send` is cross-compiled. Install
the toolchain once:

```bash
sudo apt-get install g++-arm-linux-gnueabihf
```

```bash
./scripts/build-arm.sh              # -> build-arm/fzstats_send (static, ~420K)
```

Statically linked by default so it runs on a minimal board rootfs regardless of
which libc/libstdc++ is installed there. Two smaller variants exist for when the
only way onto the board is a serial console — the script prints a ready-made
`gzip | base64` paste recipe:

```bash
./scripts/build-arm.sh --slim       # ~100K, needs only libc on the board
./scripts/build-arm.sh --dynamic    # ~14K, also needs libstdc++/libgcc_s
```

### Deploy and run

```bash
# find the board (its IP moves between boots, its MAC doesn't)
BOARD=$(./scripts/find-board.sh)

scp build-arm/fzstats_send root@$BOARD:/home/root/
ssh root@$BOARD 'chmod +x /home/root/fzstats_send'
```

Then start it, pointing at **this PC's** address and the port the analyzer listens
on. Root is required — the program reaches the PL registers through `/dev/mem`:

```bash
ssh root@$BOARD '/home/root/fzstats_send <pc-ip> 5310 \
    --n-binom 16 --n-frame 128 --fs 30720000 --fc 2400000000'
```

| Option | Meaning |
|--------|---------|
| `--n-binom B` | bins per subband (B), default 16 |
| `--n-frame M` | FFT frames accumulated per block (M), default 128 |
| `--fft N` | FFT size in the PL, default 4096 |
| `--subbands K` | how many subbands to read, default `fft/B` |
| `--fs HZ` | sample rate written into the block header |
| `--fc HZ` | centre frequency written into the block header |
| `--dc MODE` | DC filter: `bypass`, `n11`..`n16`, `off` |
| `--selftest` | spectrum-orientation check, see below |

**`--fs` and `--fc` are not cosmetic.** The PC computes each subband's frequency
from them, so wrong values silently misplace the whole frequency axis. Read the
real values off the board rather than trusting the defaults:

```bash
ssh root@$BOARD 'cat /sys/bus/iio/devices/iio:device1/in_voltage_sampling_frequency \
                     /sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency'
```

On the board this was measured at **30 720 000 Hz** with the LO at 2.4 GHz — not
the 56 MHz nominal quoted in the PDF, which would have stretched the axis by 1.8×.

### On the PC

Open the **Peleng** window, set *Statistics source* to **Board over TCP**, and
check the listen port matches (default 5310). The status reads
`waiting for board...` until the board connects, then `board connected`. The
board reconnects on its own if the analyzer restarts.

The board opens the connection *towards* the PC, so the PC must be reachable from
the board on that port. If routing is in the way, an SSH reverse tunnel works:

```bash
ssh -R 5310:localhost:5310 root@$BOARD
# then, inside that session:
/home/root/fzstats_send 172.16.226.117 5310 --fs 30720000 --fc 5806000000
```

## Python Scripts

Generate synthetic test data and analyze offline IQ captures. Synthetic generation in order to bypass hardware artifact error

**Synthetic analog-TV IQ (for testing TV video demod without hardware):**
```bash
# encode a video into an FM composite-video IQ recording, playable back via
# the app's "Play recording" IQ playback feature
python3 mp4_to_iq.py --in ../data/test_card.mp4 --out ../data/test_card_iq.bin
```

<a name="inband-calibration"></a>
**In-band channel imbalance (toward C1[p]/C2[p]):**

`data/rx_calibration.csv` v1 held one complex number per (LO, gain) — the tone it
was measured with sat at LO+1 MHz, so it described one bin out of a few hundred.
The monopulse pipeline wants the per-subband version, `C1[p]`/`C2[p]`: the shape
the analog RX filter and the decimating FIRs put *across* the instantaneous band.
Two ways to measure it, and they fail differently, which is why both exist.

**Measured, that shape turned out to be a group delay and nothing else** (fcoms,
2026-08-20, both bandwidths, cable orders half-summed): the phase grows linearly
with the offset from the LO and the amplitude is flat to within the noise. So the
table does not need a row per subband — v2 adds one `delay_ns` column, and the
per-bin correction is analytic:

```
phi[k]  = phase_deg + 360 * f_off[k] * delay_ns * 1e-9
X2'[k]  = X2[k] * 10^(-amp_db/20) * exp(-i * phi[k] * pi/180)
```

`RxCalibration::correction(freq, gain, offset_hz)` does exactly this and is what
per-bin consumers should call; the no-offset overload stays what the time-domain
capture path uses, because a delay is a filter and cannot be applied to samples
by one complex multiply. Files without the column still load, with the delay
reading zero.

**Where it is applied in the direction finder.** The split follows what each half
of the correction *is*, not where it would be convenient:

- *Local IQ.* The capture thread already rotates Rx2 by the constant, so
  `SubbandAccumulator` applies only the remaining delay, per bin, before Σ/Δ are
  formed. After the sum there is nothing left to correct.
- *Blocks from the board.* Those never passed through the capture thread and
  arrive with Σ/Δ already accumulated, so `SubbandEstimator` recomputes them from
  the covariance the PL also sends:
  `S_Σ'Σ' = S11 + |c2|²S22 + 2Re{c̄2·S12}`, `S_Δ'Σ' = S11 − |c2|²S22 + 2i·Im{c̄2·S12}`.
  No FPGA change is needed for this — the missing calibration is recoverable
  precisely because the PL transmits S11/S22/S12 alongside Σ/Δ.

Both are driven by the same **Apply Rx2 calibration** checkbox, and the Peleng
window shows whether the correction reached the pipeline and with what delay. The
CSV log records it in its header, so two logs of the same signal taken with the
correction on and off cannot be confused for two different measurements.

To look at the finished table rather than the runs behind it:

```bash
python3 rx_calibration_plot.py                  # ../data/rx_calibration.csv
python3 rx_calibration_plot.py --bandwidth 20   # per-bin panel at another bandwidth
```

It draws the constant and the amplitude over frequency with the unsmoothed values
behind them (the gap is the standing-wave ripple the generator deliberately
removes), the group delay with the LOs it was actually measured at marked, and —
the panel the other three exist for — the correction the table applies across the
instantaneous band, where a v1 table would have been a flat line.

Verified on synthetic data carrying the table's own imbalance (−32.68°, +0.83 dB,
−0.280 ns at 5450 MHz): mean |ρ| falls from 0.297 uncorrected to 0.004 on both
paths, the two paths agree to 7·10⁻⁴ per subband, and a unit correction reproduces
the uncorrected result to 3·10⁻⁸ — i.e. the recomputation is exact, not merely
close. Measured values here run −0.36 ns at 5000 MHz to −0.09 ns at 5900
— about 2° across half of a 30.72 MHz band at the low end, under 0.5° at the top.

*Stepped tone — controlled, slow.* The generator walks across the band at every
LO, so every subband is measured with a stimulus of known level and frequency:

```bash
# few LO points, many offsets: the shape comes from the baseband path, so it
# should repeat at every LO — taking three LOs is how you check that it does
python3 phase_coherence_sweep.py --inband-step-khz 250 \
    --start-mhz 5000 --stop-mhz 6000 --step-mhz 500 --gains 30
```

*Broadband source — one capture per LO, covers only what the source occupies.*
Anything wideband reaching both inputs through the same splitter works; its own
spectrum cancels in the ratio Rx2/Rx1, so it need not be flat or known. A video
transmitter cabled in is continuous and 20–40 MHz wide, so one capture covers
the band; 5 GHz Wi-Fi off an antenna costs nothing but is bursty and patchy.

**Attenuate a cabled transmitter.** A 25 mW VTX is +14 dBm and a 1 W one is +30;
the AD9361 compresses around −20 dBm and is damaged past roughly +2 dBm at the
input. Put 40–60 dB in front of the splitter and check the reported peak sits
near −10 dBFS rather than on the rail — the script says so if it does not.

```bash
python3 inband_broadband.py --lo-mhz 5800 --source drone --gain 20 --plot
python3 inband_broadband.py --lo-mhz 5800 --source drone --order swapped
```

Either way a second run with the RX cables exchanged is what separates the chip
from the splitter and the cables, exactly as for the LO sweep. The report forms
the half-sum and draws both measurements on one pair of axes:

```bash
python3 inband_report.py ../data/inband/inband_*_5800MHz_normal.npz \
    --swapped ../data/inband/inband_*_5800MHz_swapped.npz
python3 inband_report.py <sweep_run_dir> --tables      # text only
```

**How much measurement this actually needs.** The full runs above are 2928
captures; measured by thinning them down afterwards, almost all of that was
redundant, because the delay is a *slope* and a slope needs lever arm rather than
density. Reproducing the delay to within 0.13° at the edge of a 30.72 MHz band:

```bash
# ~50 captures per cable order, about a minute each, ~50 MB instead of 1.3 GB
python3 phase_coherence_sweep.py --uri ip:<board> --gen /dev/usbtmc3 \
    --sample-rate 30.72e6 --bandwidth 30.72e6 \
    --start-mhz 5000 --stop-mhz 5900 --step-mhz 450 --gains 30 \
    --inband-step-khz 1900 --buffer-size 32768 --name inband_normal
```

What each cut costs, against a correction that is worth 0.5–2.1° in the first
place:

| cut | error at the band edge |
|---|---|
| 17 offsets instead of 122 | ≤0.13° |
| 8 segments per point instead of 32 (`--buffer-size 32768`) | ≤0.03° |
| one gain instead of two | ~0.11° |
| one bandwidth instead of two | 0 — measure at the one you operate at |
| **three LO points instead of five** | **~0.34°** |
| skipping the swapped run | up to 0.67°, and it is the bench's number, not the board's |

The LO axis is the one not to economise on: the delay runs −0.36 → −0.09 ns over
900 MHz and it is visibly curved, so dropping the middle of three points already
costs more than every other cut combined. Offsets being cheap, spend the saving
there — five LO points at 17 offsets is still under a hundred captures per order.

Once a pair of in-band runs exists, feed them to the table generator and the
`delay_ns` column gets filled from them:

```bash
python3 make_rx_calibration.py \
    --normal <sweep runs> --swapped <swapped sweep> \
    --inband-normal  ../data/phase_coherence/inband_normal \
                     ../data/phase_coherence/inband30_normal \
    --inband-swapped ../data/phase_coherence/inband_swapped \
                     ../data/phase_coherence/inband30_swapped
```

Runs at several sample rates are fitted together — they cover different spans of
the same curve, and one line through the union beats averaging two slopes taken
over different lever arms. Gains are averaged (30 and 60 dB agreed to 0.02 ns
while the LO moved the delay by 0.35 ns); between measured LOs the delay
interpolates linearly and outside them it holds flat.

The report's right-hand column removes each curve's own constant and leaves only
the shape. If the shapes from different LOs lie on top of each other, the
calibration stays separable — the existing LO × gain table times one in-band
shape — instead of growing into a full LO × offset grid. `make_rx_calibration.py`
refuses in-band runs outright: that table has no offset axis, and averaging over
one would flatten the whole shape into a single meaningless number.

<a name="peleng-logging"></a>
**Peleng logging and offline analysis:**

In the Peleng window, **CSV log → Start** writes every accumulated subband block
to disk. The direction-finding thread writes it, not the render loop, so the file
holds *all* blocks rather than the one each frame happened to catch. Relative
paths land next to the executable, same as "Save IQ".

One row per subband per block:

```
# spectrum_analyzer peleng log v1
# center_hz=5800000000   sample_rate_hz=20000000
# fft_size=4096   subband_size=16   accum_frames=256   num_subbands=256
block,t_sec,subband,freq_mhz,rho_re,rho_im,gamma2,snr_db,sigma_dbfs,valid
```

A full slice is `num_subbands` rows per block at tens of blocks per second, so
tick **selected subband only** to log just the frequency picked in the Rho plane
tab when that is all you are studying.

```bash
# every characteristic averaged over the whole log, versus frequency
python3 peleng_analysis.py ../data/peleng_log.csv

# one frequency: rho over time, gamma^2/SNR, and the complex-plane cloud
python3 peleng_analysis.py ../data/peleng_log.csv --freq 5800.5

# the cloud is often orders of magnitude smaller than rho itself — --zoom
# frames it instead of keeping the origin in view
python3 peleng_analysis.py ../data/peleng_log.csv --freq 5800.5 --zoom

# cut one leg of a manual sweep out by time, drop estimator-masked rows
python3 peleng_analysis.py ../data/peleng_log.csv --freq 5800.5 --from 10 --to 25 --valid

# per-subband summary table instead of plots
python3 peleng_analysis.py ../data/peleng_log.csv --stats
```

---

## Project Structure

```
spectrum_analyzer/
├── include/
│   ├── app_state.hpp        # shared state between all threads
│   ├── circular_buffer.hpp  # lock-free SPSC ring buffer
│   ├── sample.hpp           # CoherentSample (RX1/RX2 pair)
│   ├── capture.hpp          # PlutoCaptureThread — AD9361/libiio capture
│   ├── processing.hpp       # FFT/Welch PSD processing thread
│   ├── peleng.hpp           # monopulse direction-finding DSP (PL/PS split)
│   ├── finding.hpp          # DirectionFindingThread — local IQ or board stats, publishes results + CSV log
│   ├── paths.hpp            # data-file paths resolved next to the executable
│   ├── rx_calibration.hpp   # Rx2-vs-Rx1 imbalance table loaded from data/rx_calibration.csv
│   ├── subband_wire.hpp     # board<->PC wire format, shared verbatim with the board build
│   ├── network_stats.hpp    # NetworkSubbandReceiver — TCP server for PL statistics
│   ├── board_finder.hpp     # BoardFinderThread — locates the board by MAC
│   ├── clip_monitor.hpp     # ADC clipping counter shared by capture, playback, and Save IQ
│   ├── dsp_utils.hpp        # FIR design/decimation, FM discriminator, resampler
│   ├── demod.hpp            # DemodThread — FM audio + TV video demod
│   ├── audio_output.hpp     # miniaudio playback device wrapper
│   ├── playback.hpp         # IqPlaybackThread — replays recorded .bin files
│   ├── tv_recorder.hpp      # TvRecorderThread — writes decoded TV frames to disk
│   └── display.hpp          # ImGui/ImPlot display thread (main + peleng + TV windows)
├── src/
│   ├── main.cpp              # thread launch and shutdown
│   ├── paths.cpp
│   ├── circular_buffer.cpp
│   ├── capture.cpp
│   ├── clip_monitor.cpp
│   ├── rx_calibration.cpp
│   ├── processing.cpp
│   ├── peleng.cpp
│   ├── finding.cpp
│   ├── network_stats.cpp
│   ├── board_finder.cpp
│   ├── dsp_utils.cpp
│   ├── demod.cpp
│   ├── audio_output.cpp
│   ├── playback.cpp
│   ├── tv_recorder.cpp
│   └── display.cpp
├── python/
│   ├── psd_plot.py          # offline PSD analysis
│   ├── psd_generate.py      # synthetic PSD test data
│   ├── peleng_analysis.py   # offline analysis of a peleng CSV log
│   ├── mp4_to_iq.py         # synthetic analog-TV IQ from a video file
│   ├── iq_tv_decode.py      # offline TV decode of an IQ capture to PNG/mp4
│   ├── phase_coherence_sweep.py   # Rx1/Rx2 phase sweep driving the PLG12-12F generator
│   ├── phase_coherence_report.py  # tables and charts for one sweep run
│   ├── iq_capture_view.py         # inspects a single capture from a sweep run
│   ├── make_rx_calibration.py     # turns sweeps into data/rx_calibration.csv
│   ├── inband_broadband.py        # in-band Rx2/Rx1 imbalance from a broadband source
│   ├── inband_report.py           # in-band curves from either measurement, side by side
│   ├── rx_calibration_plot.py     # draws the finished table, not the sweeps behind it
│   └── plg_control.py             # manual control panel for the PLG12-12F generator
├── tools/
│   └── tv_stream_check.cpp  # offline TV decoder check (target tv_stream_check)
├── FZTest/                  # board side (Zynq PS)
│   ├── cpp/
│   │   ├── mmap.hpp         # /dev/mem mapping
│   │   ├── axil.hpp         # AXI-Lite 32-bit accessors
│   │   ├── bctrl.hpp        # clock/reset controller (0x43D00000)
│   │   ├── zctrl.hpp        # DSP controller + statistics BRAM (0x43C00000)
│   │   └── fzstats_send.cpp # reads PL statistics, streams them to the PC
│   └── *.zig                # original Zig reference the C++ port follows
├── fft_stat/                # board side (Zynq PL) — Vivado TCL for the FFT/statistics IP
├── cmake/
│   └── toolchain-mingw64.cmake  # mingw-w64 cross-compile toolchain
├── scripts/
│   ├── build-windows.sh     # cross-build spectrum_analyzer.exe
│   ├── build-arm.sh         # cross-build fzstats_send for the board
│   ├── find-board.sh        # locate the board by mDNS / MAC
│   └── board/               # run on the board itself
│       ├── net-dhcp.sh      # switch the wired interface to DHCP, persistently
│       └── net-static.sh    # pin a static IP on the wired interface, persistently
├── data/                    # IQ captures saved here (gitignored except rx_calibration.csv)
├── lib/                     # libiio + libad9361-iio source (Windows cross-build only)
└── third_party/             # imgui, implot, miniaudio (gitignored, fetched per Dependencies)
```

---

## Tools and Libraries

| Tool | Purpose |
|------|---------|
| libiio | Hardware abstraction / IIO context for AD9361-based SDRs |
| libad9361-iio | AD9361-specific helpers (RF bandwidth, LO tuning) on top of libiio |
| FFTW3 (float) | Fast Fourier Transform engine |
| Dear ImGui | Immediate mode GUI framework |
| ImPlot | GPU-accelerated plotting for ImGui |
| GLFW + OpenGL 3.3 | Window and rendering backend |
| miniaudio | Cross-platform audio playback for demodulated FM/TV audio |
| NumPy / Matplotlib | Offline IQ analysis and visualization |

---

## References

- [PlutoSDR / ADALM-PLUTO Documentation](https://wiki.analog.com/university/tools/pluto)
- [libiio](https://github.com/analogdevicesinc/libiio)
- [libad9361-iio](https://github.com/analogdevicesinc/libad9361-iio)
- [FFTW3 Documentation](https://fftw.org/fftw3_doc/)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [ImPlot](https://github.com/epezent/implot)
- [miniaudio](https://miniaud.io/)