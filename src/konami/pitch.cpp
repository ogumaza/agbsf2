// SPDX-License-Identifier: MIT

#include "konami/pitch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "konami/driver.h"
#include "rom.h"

namespace agbsf2::konami
{
namespace
{

constexpr double kMinHz = 25.0;
constexpr double kMaxHz = 2500.0;
constexpr int kBufferSize = 4096;

// YIN's absolute threshold: the period is at the first dip of the difference function below it.
constexpr double kThreshold = 0.12;

// The highest value the deepest dip can have when no dip reaches kThreshold. A sample whose deepest dip is higher has
// no clear pitch.
constexpr double kMaxDip = 0.3;

// Estimates the fundamental frequency of `x`, sampled at `rate` Hz, with YIN (de Cheveigne & Kawahara, 2002), the
// cumulative mean normalised difference method. Returns it in Hz, or 0 if it finds no clear pitch.
double Yin(const std::vector<float>& x, double rate)
{
    const int window = int(x.size()) / 2;
    const int tau_min = std::max(2, int(rate / kMaxHz));
    const int tau_max = std::min(window - 1, int(std::ceil(rate / kMinHz)));
    if (tau_max <= tau_min + 2)
    {
        return 0;
    }

    std::vector<double> cmnd(size_t(tau_max) + 2, 1.0);
    double running = 0;
    for (int tau = 1; tau <= tau_max + 1; tau++)
    {
        double d = 0;
        for (int j = 0; j < window; j++)
        {
            const double diff = x[size_t(j)] - x[size_t(j + tau)];
            d += diff * diff;
        }

        running += d;
        cmnd[size_t(tau)] = running > 0 ? d * tau / running : 1.0;
    }

    int best = -1;
    for (int tau = tau_min; tau <= tau_max; tau++)
    {
        if (cmnd[size_t(tau)] < kThreshold)
        {
            while (tau + 1 <= tau_max && cmnd[size_t(tau + 1)] < cmnd[size_t(tau)])
            {
                tau++;
            }

            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = tau_min;
        for (int tau = tau_min; tau <= tau_max; tau++)
        {
            if (cmnd[size_t(tau)] < cmnd[size_t(best)])
            {
                best = tau;
            }
        }
        if (cmnd[size_t(best)] > kMaxDip)
        {
            return 0;
        }
    }

    // Parabolic interpolation of the dip.
    double tau = best;
    const double a = cmnd[size_t(best - 1)], b = cmnd[size_t(best)], c = cmnd[size_t(best + 1)];
    const double denom = a - 2 * b + c;
    if (denom > 1e-12)
    {
        tau += 0.5 * (a - c) / denom;
    }

    return tau > 0 ? rate / tau : 0;
}

} // namespace

double EstimateSamplePitch(const Rom& rom, const SampleInfo& sample, double rate)
{
    if (!sample.valid || sample.length <= 0 || rate <= 0)
    {
        return 0;
    }

    const int len = sample.length;
    std::vector<float> buf;
    buf.reserve(kBufferSize);
    if (sample.Looped() && len - sample.loop_start >= 8)
    {
        // The loop is the steady state: repeat it to fill the analysis buffer.
        const int loop_len = len - sample.loop_start;
        for (int i = 0; int(buf.size()) < kBufferSize; i++)
        {
            buf.push_back(float(rom.S8(sample.data + uint32_t(sample.loop_start + i % loop_len))));
        }
    }
    else
    {
        // Skip the attack of one-shot samples.
        const int start = std::min(len / 4, 1024);
        const int n = std::min(len - start, kBufferSize);
        if (n < 1024)
        {
            return 0;
        }

        for (int i = 0; i < n; i++)
        {
            buf.push_back(float(rom.S8(sample.data + uint32_t(start + i))));
        }
    }

    double mean = 0;
    for (float v : buf)
    {
        mean += v;
    }
    mean /= double(buf.size());

    double energy = 0;
    for (float& v : buf)
    {
        v = float(v - mean);
        energy += double(v) * v;
    }
    if (energy / double(buf.size()) < 1.0)
    {
        return 0; // near silence
    }

    return Yin(buf, rate);
}

int NearestKey(double hz)
{
    if (hz <= 0)
    {
        return 60;
    }

    return std::clamp(int(std::lround(69.0 + 12.0 * std::log2(hz / 440.0))), 0, 127);
}

} // namespace agbsf2::konami
