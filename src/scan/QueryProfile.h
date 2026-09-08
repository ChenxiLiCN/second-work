#ifndef HINSCAN_QUERY_PROFILE_H
#define HINSCAN_QUERY_PROFILE_H
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>

namespace hinscan::profile {
enum class Phase { Setup, Prune, Core, Noncore, Count };
enum class Part { Candidates, Activation, State, Exact, CertificateGet, CertificatePut,
                  CertificateRehash, Find, Unite, AdaptiveCheck, WitnessBounds,
                  WitnessCount, FullIntersection, Count };
#ifdef HINSCAN_QUERY_PROFILE
using Clock = std::chrono::steady_clock;
struct Record {
    std::uint64_t calls=0, samples=0;
    double ns=0, ns_squared=0;
    std::uint32_t random=1;
};
inline constexpr std::size_t parts=static_cast<std::size_t>(Part::Count);
inline thread_local std::array<Record, parts*static_cast<std::size_t>(Phase::Count)> records;
inline thread_local Phase phase=Phase::Setup;
inline thread_local std::uint32_t seed=1;
inline void reset() {
    records={}; phase=Phase::Setup;
    seed=static_cast<std::uint32_t>(Clock::now().time_since_epoch().count()) | 1U;
    for (std::size_t i=0; i<records.size(); ++i)
        records[i].random=(seed + static_cast<std::uint32_t>(i+1)*2654435761U) | 1U;
}
inline void set_phase(Phase value) { phase=value; }
constexpr unsigned stride(Part p) {
    return p==Part::Candidates || p==Part::Activation || p==Part::CertificateRehash ||
           p==Part::FullIntersection ? 1 : 256;
}
constexpr bool enabled(Part p) {
#if HINSCAN_QUERY_PROFILE == 1
    return p==Part::Candidates || p==Part::Activation || p==Part::AdaptiveCheck ||
           p==Part::WitnessBounds || p==Part::WitnessCount || p==Part::FullIntersection;
#else
    return p==Part::State || p==Part::Exact || p==Part::CertificateGet ||
           p==Part::CertificatePut || p==Part::CertificateRehash || p==Part::Find || p==Part::Unite;
#endif
}
template<Part P> class Scope {
    Record* record_=nullptr;
    Clock::time_point start_;
public:
    Scope() {
        if constexpr (!enabled(P)) return;
        auto& r=records[static_cast<std::size_t>(phase)*parts+static_cast<std::size_t>(P)];
        ++r.calls;
        if constexpr (stride(P)>1) {
            auto x=r.random; x^=x<<13; x^=x>>17; x^=x<<5; r.random=x;
            if ((x & (stride(P)-1))!=0) return;
        }
        record_=&r;
        start_=Clock::now();
    }
    ~Scope() {
        if (!record_) return;
        const auto end=Clock::now();
        const double ns=std::chrono::duration<double,std::nano>(end-start_).count();
        ++record_->samples; record_->ns+=ns; record_->ns_squared+=ns*ns;
    }
};
inline void write(std::ostream& out) {
    constexpr const char* phases[]={"setup","prune","core","noncore"};
    constexpr const char* names[]={"candidates","activation","state","exact",
        "certificate_get","certificate_put","certificate_rehash","find","unite",
        "adaptive_check","witness_bounds","witness_count","full_intersection"};
    double empty_ns=0;
    for (unsigned i=0; i<10000; ++i) {
        const auto a=Clock::now(), b=Clock::now();
        empty_ns+=std::chrono::duration<double,std::nano>(b-a).count();
    }
    const double clock_ns=empty_ns/10000;
    out << "profile_group=" << HINSCAN_QUERY_PROFILE << "\nprofile_seed=" << seed << "\nprofile_empty_clock_ns=" << clock_ns << '\n';
    for (std::size_t i=0; i<records.size(); ++i) {
        const auto& r=records[i]; if (!r.calls) continue;
        const auto s=stride(static_cast<Part>(i%parts));
        const auto prefix=std::string("profile_")+phases[i/parts]+"_"+names[i%parts];
        out << prefix << "_calls=" << r.calls << '\n'
            << prefix << "_samples=" << r.samples << '\n'
            << prefix << "_inclusive_ms=" << r.ns*s/1e6 << '\n'
            << prefix << "_clock_adjusted_ms=" << (r.ns-clock_ns*r.samples)*s/1e6 << '\n'
            << prefix << "_sampling_se_ms=" << std::sqrt((1.0-1.0/s)*r.ns_squared)*s/1e6 << '\n';
    }
}
#else
inline void reset() {}
inline void set_phase(Phase) {}
template<Part> class Scope {};
inline void write(std::ostream&) {}
#endif
}
#define HIN_PROFILE_JOIN_(a,b) a##b
#define HIN_PROFILE_JOIN(a,b) HIN_PROFILE_JOIN_(a,b)
#ifdef HINSCAN_QUERY_PROFILE
#define HIN_PROFILE_SCOPE(part) hinscan::profile::Scope<hinscan::profile::Part::part> HIN_PROFILE_JOIN(query_profile_,__LINE__)
#else
#define HIN_PROFILE_SCOPE(part) ((void)0)
#endif
#endif
