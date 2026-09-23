#include "DirectLut.h"

#include "PhaseMath.h"

#include <cmath>

namespace afar::cal {

DirectLutEntry build_direct_lut_entry(const DirectLutBuildInput& in)
{
    DirectLutEntry e;
    e.channel = in.channel;
    e.freq_hz = in.freq_hz;
    e.att_code = in.att_code;
    e.phase_code = in.phase_code;
    e.s21_re = in.s_tilde.real();
    e.s21_im = in.s_tilde.imag();
    e.mag_db = magnitude_db(in.s_tilde);
    e.phase_unwrapped_deg = in.phase_unwrapped_deg;
    e.atten_meas_db = attenuation_db(in.s_tilde);
    e.phase_error_deg = wrap180(in.phase_unwrapped_deg - in.nominal_phase_deg);
    e.drift_phase_deg = in.drift_phase_deg;
    e.repeatability_db = in.repeatability_db;
    e.repeatability_deg = in.repeatability_deg;

    const bool finite_complex =
        std::isfinite(e.s21_re) && std::isfinite(e.s21_im);
    const bool finite_mag =
        std::isfinite(e.mag_db) && std::isfinite(e.atten_meas_db);
    e.valid = in.sample_valid && finite_complex && finite_mag
        && std::isfinite(e.phase_unwrapped_deg);
    return e;
}

}  // namespace afar::cal
