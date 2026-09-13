#include "index/MajorityIndex.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
namespace hinscan {
namespace {
constexpr std::uint64_t magic=0x3149474d4e4948ULL, header_size=88;
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) throw std::overflow_error("MGI size overflow");
    return a+b;
}
std::uint64_t mul(std::uint64_t a,std::uint64_t b) {
    if(b && a>std::numeric_limits<std::uint64_t>::max()/b) throw std::overflow_error("MGI size overflow");
    return a*b;
}
struct Crc {
    std::uint64_t value=0;
    void update(const char* p,std::size_t n) {
        static const auto table=[] {
            std::array<std::uint64_t,256> t{};
            for(unsigned i=0;i<256;++i) {
                auto c=static_cast<std::uint64_t>(i)<<56;
                for(unsigned b=0;b<8;++b) c=(c>>63)?(c<<1)^0x42F0E1EBA9EA3693ULL:c<<1;
                t[i]=c;
            }
            return t;
        }();
        for(std::size_t i=0;i<n;++i) value=table[(value>>56)^static_cast<unsigned char>(p[i])]^(value<<8);
    }
    template<class T> void scalar(T v) {update(reinterpret_cast<const char*>(&v),sizeof(v));}
};
void endian_check() {
    const std::uint32_t x=1;
    if(*reinterpret_cast<const unsigned char*>(&x)!=1) throw std::runtime_error("MGI requires little endian");
}
std::uint64_t fingerprint(const HinGraph& g) {
    Crc c;
    c.scalar(static_cast<std::uint64_t>(g.vertex_types().size()));
    for(const auto& t:g.vertex_types()) {
        c.scalar(t.count);c.scalar(static_cast<std::uint64_t>(t.name.size()));c.update(t.name.data(),t.name.size());
    }
    c.scalar(static_cast<std::uint64_t>(g.relations().size()));
    for(const auto& r:g.relations()) {
        c.scalar(r.source_type);c.scalar(r.target_type);
        for(const auto* lists:{&r.forward,&r.reverse}) for(const auto& row:*lists) {
            c.scalar(static_cast<std::uint64_t>(row.size()));
            if(!row.empty()) c.update(reinterpret_cast<const char*>(row.data()),row.size()*sizeof(VertexId));
        }
    }
    return c.value;
}
using Groups=std::map<std::pair<std::uint64_t,std::uint64_t>,std::vector<VertexId>>;
// Traverse only intervals with actual neighbors; emit each row's membership once.
void collect(const std::vector<VertexId>& row,std::size_t first,std::size_t last,
             std::uint64_t lo,std::uint64_t hi,VertexId u,Groups& groups) {
    if(first==last) return;
    if(2ULL*(last-first)>hi-lo) groups[{lo,hi}].push_back(u);
    if(hi-lo==1) return;
    const auto mid=lo+(hi-lo)/2;
    const auto split=static_cast<std::size_t>(std::lower_bound(row.begin()+first,row.begin()+last,mid)-row.begin());
    collect(row,first,split,lo,mid,u,groups);
    collect(row,split,last,mid,hi,u,groups);
}
MajorityDirection build_direction(const std::vector<std::vector<VertexId>>& rows,std::uint64_t target) {
    std::uint64_t width=1;while(width<target) width*=2;
    Groups groups;
    for(std::size_t u=0;u<rows.size();++u) collect(rows[u],0,rows[u].size(),0,width,static_cast<VertexId>(u),groups);
    MajorityDirection d;
    for(const auto& entry:groups) {
        d.members.insert(d.members.end(),entry.second.begin(),entry.second.end());
        d.offsets.push_back(d.members.size());
    }
    return d;
}
std::uint64_t payload_size(const std::vector<MajorityDirection>& ds) {
    std::uint64_t n=0;
    for(const auto& d:ds) n=add(n,add(48,add(mul(d.offsets.size(),8),mul(d.members.size(),4))));
    return n;
}
// CRC pass and write pass stream existing arrays, avoiding a full payload copy.
template<class F> void payload(const HinGraph& graph,const std::vector<MajorityDirection>& ds,F&& send) {
    for(std::size_t i=0;i<ds.size();++i) {
        const auto& r=graph.relations()[i/2];
        const auto a=i%2?r.target_type:r.source_type,b=i%2?r.source_type:r.target_type;
        const auto& d=ds[i];
        const std::array<std::uint64_t,6> h{a,b,graph.vertex_types()[a].count,graph.vertex_types()[b].count,d.offsets.size()-1,d.members.size()};
        send(reinterpret_cast<const char*>(h.data()),sizeof(h));
        send(reinterpret_cast<const char*>(d.offsets.data()),d.offsets.size()*8);
        if(!d.members.empty()) send(reinterpret_cast<const char*>(d.members.data()),d.members.size()*4);
    }
}
}
MajorityIndex MajorityIndex::build(const HinGraph& graph) {
    endian_check();MajorityIndex out;out.graph_=&graph;out.fingerprint_=fingerprint(graph);
    for(const auto& r:graph.relations()) {
        out.directions_.push_back(build_direction(r.forward,r.reverse.size()));
        out.directions_.push_back(build_direction(r.reverse,r.forward.size()));
    }
    return out;
}
void MajorityIndex::save(const std::filesystem::path& file,const HinGraph& graph) const {
    if(graph_!=&graph) throw std::invalid_argument("MGI graph binding mismatch");
    if(std::filesystem::exists(file)) throw std::runtime_error("MGI output exists");
    Crc crc;payload(graph,directions_,[&](const char* p,std::size_t n){crc.update(p,n);});
    // 88-byte header: magic, version/endian, types, relations, graph CRC,
    // direction/group/member counts, payload bytes, reserved, payload CRC.
    const std::array<std::uint64_t,11> h{magic,(0x01020304ULL<<32)|1,
        graph.vertex_types().size(),graph.relations().size(),fingerprint_,directions_.size(),
        group_count(),member_count(),payload_size(directions_),0,crc.value};
    std::ofstream output(file,std::ios::binary);
    auto write=[&](const char* p,std::size_t n) {
        output.write(p,static_cast<std::streamsize>(n));if(!output) throw std::runtime_error("MGI write failed");
    };
    write(reinterpret_cast<const char*>(h.data()),sizeof(h));payload(graph,directions_,write);
    output.close();if(!output) throw std::runtime_error("MGI close failed");
}
MajorityIndex MajorityIndex::load(const std::filesystem::path& file,const HinGraph& graph) {
    endian_check();const auto bytes=std::filesystem::file_size(file);
    if(bytes<header_size) throw std::runtime_error("truncated MGI header");
    std::ifstream input(file,std::ios::binary);std::array<std::uint64_t,11> h{};
    input.read(reinterpret_cast<char*>(h.data()),sizeof(h));
    if(!input || h[0]!=magic || h[1]!=((0x01020304ULL<<32)|1) || h[9]!=0 ||
       h[2]!=graph.vertex_types().size() || h[3]!=graph.relations().size() ||
       h[5]!=2*graph.relations().size() || h[8]!=bytes-header_size)
        throw std::runtime_error("invalid MGI header/dimensions");
    MajorityIndex out;out.graph_=&graph;out.fingerprint_=fingerprint(graph);
    if(h[4]!=out.fingerprint_) throw std::runtime_error("MGI graph fingerprint mismatch");
    std::uint64_t remaining=h[8];Crc crc;
    auto read=[&](char* p,std::size_t n) {
        if(n>remaining) throw std::runtime_error("truncated MGI payload");
        input.read(p,static_cast<std::streamsize>(n));if(!input) throw std::runtime_error("MGI read failed");
        crc.update(p,n);remaining-=n;
    };
    for(std::size_t i=0;i<h[5];++i) {
        std::array<std::uint64_t,6> dh{};read(reinterpret_cast<char*>(dh.data()),sizeof(dh));
        const auto& r=graph.relations()[i/2];
        const auto a=i%2?r.target_type:r.source_type,b=i%2?r.source_type:r.target_type;
        if(dh[0]!=a || dh[1]!=b || dh[2]!=graph.vertex_types()[a].count || dh[3]!=graph.vertex_types()[b].count)
            throw std::runtime_error("MGI direction mismatch");
        const auto ob=mul(add(dh[4],1),8),mb=mul(dh[5],4);
        if(add(ob,mb)>remaining || dh[5]>std::numeric_limits<std::size_t>::max()/4 || dh[4]>=std::numeric_limits<std::size_t>::max()/8)
            throw std::runtime_error("invalid MGI allocation size");
        MajorityDirection d;d.offsets.resize(static_cast<std::size_t>(dh[4]+1));d.members.resize(static_cast<std::size_t>(dh[5]));
        read(reinterpret_cast<char*>(d.offsets.data()),static_cast<std::size_t>(ob));
        if(mb) read(reinterpret_cast<char*>(d.members.data()),static_cast<std::size_t>(mb));
        if(d.offsets.front()!=0 || d.offsets.back()!=dh[5]) throw std::runtime_error("invalid MGI offsets");
        for(std::size_t g=0;g+1<d.offsets.size();++g) {
            if(d.offsets[g]>=d.offsets[g+1] || d.offsets[g+1]>dh[5]) throw std::runtime_error("invalid or empty MGI group");
            const auto begin=d.members.begin()+d.offsets[g],end=d.members.begin()+d.offsets[g+1];
            if(!std::is_sorted(begin,end) || std::adjacent_find(begin,end)!=end || *(end-1)>=dh[2])
                throw std::runtime_error("invalid MGI members");
        }
        out.directions_.push_back(std::move(d));
    }
    if(remaining || crc.value!=h[10] || out.group_count()!=h[6] || out.member_count()!=h[7])
        throw std::runtime_error("MGI checksum/count mismatch");
    return out;
}
const MajorityDirection& MajorityIndex::groups_for(const HinGraph& graph,const Transition& t) const {
    if(graph_!=&graph || !t.relation) throw std::invalid_argument("MGI graph binding mismatch");
    for(std::size_t i=0;i<graph.relations().size();++i)
        if(&graph.relations()[i]==t.relation) return directions_.at(2*i+static_cast<std::size_t>(t.use_reverse));
    throw std::invalid_argument("MGI transition binding mismatch");
}
std::uint64_t MajorityIndex::group_count() const noexcept {std::uint64_t n=0;for(const auto& d:directions_) n+=d.offsets.size()-1;return n;}
std::uint64_t MajorityIndex::member_count() const noexcept {std::uint64_t n=0;for(const auto& d:directions_) n+=d.members.size();return n;}
}
