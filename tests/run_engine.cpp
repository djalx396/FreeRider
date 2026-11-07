// Offline harness: runs RiderEngine over raw float32 files and writes a 1 kHz trace.
//   run_engine vocal.f32 music.f32|- sr out.txt [key=value ...]
// keys: target vsens msens min max idle attack(0 slow/1 fast) block
// trace columns: time_s  applied_dB  auto_dB  level_dB  riding  music_dB  targetEff
#include "../Source/RiderEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<float> readRaw (const char* p)
{
    std::vector<float> v;
    if (std::strcmp (p, "-") == 0) return v;
    FILE* f = std::fopen (p, "rb");
    if (! f) { std::perror (p); std::exit (1); }
    std::fseek (f, 0, SEEK_END); long n = std::ftell (f) / 4; std::fseek (f, 0, SEEK_SET);
    v.resize ((size_t) n);
    if (std::fread (v.data(), 4, (size_t) n, f) != (size_t) n) std::exit (2);
    std::fclose (f);
    return v;
}

int main (int argc, char** argv)
{
    if (argc < 5) { std::fprintf (stderr, "usage\n"); return 1; }
    auto voc = readRaw (argv[1]);
    auto mus = readRaw (argv[2]);   // stereo interleaved
    const double sr = std::atof (argv[3]);
    rider::Settings s;
    int block = 512;
    for (int a = 5; a < argc; ++a)
    {
        std::string kv = argv[a]; auto eq = kv.find ('=');
        std::string k = kv.substr (0, eq); double v = std::atof (kv.c_str() + eq + 1);
        if (k == "target") s.targetDb = v; else if (k == "vsens") s.vocalSens = v;
        else if (k == "msens") s.musicSens = v; else if (k == "min") s.rangeMin = v;
        else if (k == "max") s.rangeMax = v; else if (k == "idle") s.idleDb = v;
        else if (k == "attack") s.attack = v > 0.5 ? rider::AttackMode::Fast : rider::AttackMode::Slow;
        else if (k == "block") block = (int) v;
    }
    rider::Engine e; e.prepare (sr);
    rider::Telemetry t;
    FILE* out = std::fopen (argv[4], "w");
    const size_t N = voc.size();
    std::vector<float> buf ((size_t) block), ml ((size_t) block), mr ((size_t) block);
    const int dec = (int) (sr / 1000.0);
    size_t pos = 0;
    while (pos < N)
    {
        const int n = (int) std::min<size_t> ((size_t) block, N - pos);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = voc[pos + (size_t) i];
        const bool haveMus = mus.size() >= 2 * N;
        if (haveMus) for (int i = 0; i < n; ++i) { ml[(size_t) i] = mus[2 * (pos + (size_t) i)]; mr[(size_t) i] = mus[2 * (pos + (size_t) i) + 1]; }
        // run sample-by-sample in sub-blocks of 1 ms so the trace is exact
        for (int i0 = 0; i0 < n; i0 += dec)
        {
            const int m = std::min (dec, n - i0);
            float* io[1] = { buf.data() + i0 };
            const float* sc[2] = { ml.data() + i0, mr.data() + i0 };
            e.process (io, 1, haveMus ? sc : nullptr, haveMus ? 2 : 0, m, s, &t);
            std::fprintf (out, "%.4f %.3f %.3f %.2f %d %.2f %.2f\n", (double) (pos + (size_t) i0 + (size_t) m) / sr,
                          t.riderDb.load(), t.autoDb.load(), t.levelDb.load(), (int) t.riding.load(), t.musicDb.load(), t.targetEff.load());
        }
        pos += (size_t) n;
    }
    std::fclose (out);
    return 0;
}
