#include "sha256.hpp"

#include <bit>
#include <cstdint>

namespace cw::server::core
{
namespace
{
constexpr std::array<std::uint32_t, 64> constants{
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

void transform(std::array<std::uint32_t, 8>& state, const std::array<std::byte, 64>& block) noexcept
{
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i=0;i<16;++i) { const auto o=i*4; words[i]=(std::to_integer<std::uint32_t>(block[o])<<24)|(std::to_integer<std::uint32_t>(block[o+1])<<16)|(std::to_integer<std::uint32_t>(block[o+2])<<8)|std::to_integer<std::uint32_t>(block[o+3]); }
    for (std::size_t i=16;i<64;++i) { const auto s0=std::rotr(words[i-15],7)^std::rotr(words[i-15],18)^(words[i-15]>>3); const auto s1=std::rotr(words[i-2],17)^std::rotr(words[i-2],19)^(words[i-2]>>10); words[i]=words[i-16]+s0+words[i-7]+s1; }
    auto a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for(std::size_t i=0;i<64;++i){const auto s1=std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25);const auto ch=(e&f)^(~e&g);const auto t1=h+s1+ch+constants[i]+words[i];const auto s0=std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22);const auto maj=(a&b)^(a&c)^(b&c);const auto t2=s0+maj;h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}
} // namespace

sha256_digest sha256(std::string_view bytes) noexcept
{
    std::array<std::uint32_t,8> state{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<std::byte,64> block{}; std::size_t used=0;
    for(const unsigned char value:bytes){block[used++]=static_cast<std::byte>(value);if(used==64){transform(state,block);block.fill({});used=0;}}
    block[used++]=std::byte{0x80};if(used>56){transform(state,block);block.fill({});}
    const auto bits=static_cast<std::uint64_t>(bytes.size())*8u;for(std::size_t i=0;i<8;++i)block[63-i]=static_cast<std::byte>((bits>>(i*8))&0xffu);transform(state,block);
    sha256_digest result;for(std::size_t i=0;i<8;++i)for(std::size_t j=0;j<4;++j)result[i*4+j]=static_cast<std::byte>((state[i]>>(24-j*8))&0xffu);return result;
}
} // namespace cw::server::core
