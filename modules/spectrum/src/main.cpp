#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "capture.hpp"
#include "processing.hpp"
#include "finding.hpp"
#include "board_finder.hpp"
#include "demod.hpp"
#include "audio_output.hpp"
#include "playback.hpp"
#include "tv_recorder.hpp"
#include "display.hpp"

#include <thread>
#include <iostream>

#ifndef _WIN32
#include <csignal>
#endif


int main() {
    std::cout << "[main] starting\n";

#ifndef _WIN32
    // TvRecorderThread writes frames into an ffmpeg pipe. If ffmpeg dies (bad
    // output path, missing codec, disk full), the default SIGPIPE disposition
    // would kill the whole process on the next write instead of letting
    // fwrite fail with EPIPE — which the recorder already handles by stopping
    // the recording and raising tv_record_error.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    AppState state;

    // 2^18 samples ~ 6.5ms of headroom at 40 MSPS
    CircularBuffer<CoherentSample> fft_buf(1 << 18);
    CircularBuffer<CoherentSample> df_buf(1 << 18);

    // demod_buf gets extra headroom (2^23 ~ 210ms @ 40 MSPS, ~420ms @ 20 MSPS)
    // vs. fft_buf/df_buf above: TV video mode's per-sample sync/line state
    // machine plus the NCO-mix+FIR+discriminator chroma and audio-subcarrier
    // passes are far heavier per input sample than PSD/peleng, so it's the
    // consumer most likely to fall behind under transient CPU contention —
    // measured (see color_diag-style offline harness) at only ~1% of
    // sustained real-time margin for NTSC color + audio subcarrier at 20
    // MSPS, so even a brief stall from another thread (display/capture/OS
    // scheduling) can push it behind. A bigger ring buffer means that stall
    // drains from backlog instead of push() silently overwriting unread
    // samples — which was showing up as periodic glitches/tearing in the
    // decoded picture. (2^21, ~105ms, wasn't enough headroom in practice.)
    CircularBuffer<CoherentSample> demod_buf(1 << 23);

    // ~1.4s of headroom at 48kHz mono — audio callback cadence is much
    // gentler than the raw IQ ring buffers above
    CircularBuffer<float> audio_out_buf(1 << 16);

    PlutoCaptureThread     capture   (fft_buf, df_buf, demod_buf, state);
    ProcessingThread       processing(fft_buf, state);
    DirectionFindingThread finding   (df_buf, state);
    DemodThread            demod     (demod_buf, audio_out_buf, state);
    AudioOutputDevice      audio_out (audio_out_buf, state);
    IqPlaybackThread       playback  (fft_buf, df_buf, demod_buf, state);
    TvRecorderThread       tv_recorder(state);
    BoardFinderThread      board_finder(state);
    DisplayThread          display   (state);

    std::thread capture_thread    ([&capture]()    { capture.run();    });
    std::thread processing_thread ([&processing]() { processing.run(); });
    std::thread finding_thread    ([&finding]()    { finding.run();    });
    std::thread demod_thread      ([&demod]()      { demod.run();      });
    std::thread playback_thread   ([&playback]()   { playback.run();   });
    std::thread tv_recorder_thread([&tv_recorder](){ tv_recorder.run();});
    std::thread board_finder_thread([&board_finder](){ board_finder.run(); });

    // miniaudio runs its own realtime callback thread once started — not a
    // std::thread we own/join. A missing audio backend is non-fatal.
    if (!audio_out.start())
        std::cerr << "[main] audio output device failed to start — continuing without audio\n";

    display.run(); // blocks until window closed

    state.running.store(false);
    capture_thread.join();
    processing_thread.join();
    finding_thread.join();
    demod_thread.join();
    playback_thread.join();
    tv_recorder_thread.join();
    board_finder_thread.join();

    std::cout << "[main] shutdown complete\n";
    return 0;
}
