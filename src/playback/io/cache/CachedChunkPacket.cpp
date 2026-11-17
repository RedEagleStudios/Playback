#include "CachedChunkPacket.h"

#include "mbedtls/sha3.h"
#include "xxhash.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace playback::io {

CachedChunkPacket::CachedChunkPacket(int32_t packetId, std::string_view payload, int index)
: mBigHash(computePacketBigHash(packetId, payload)),
  mIndex(index) {
    mLongHashCode = XXH3_64bits(mBigHash.data(), mBigHash.size());
}

std::array<uint8_t, 64> CachedChunkPacket::computePacketBigHash(int32_t packetId, std::string_view payload) {
    std::array<uint8_t, 64> result{};
    mbedtls_sha3_context    ctx;
    mbedtls_sha3_init(&ctx);
    bool const ok = mbedtls_sha3_starts(&ctx, MBEDTLS_SHA3_512) == 0
                 && mbedtls_sha3_update(&ctx, reinterpret_cast<uint8_t const*>(&packetId), sizeof(packetId)) == 0
                 && mbedtls_sha3_update(&ctx, reinterpret_cast<uint8_t const*>(payload.data()), payload.size()) == 0
                 && mbedtls_sha3_finish(&ctx, result.data(), result.size()) == 0;
    mbedtls_sha3_free(&ctx);
    if (!ok) {
        throw std::runtime_error("Failed to compute the SHA3-512 packet hash");
    }
    return result;
}

bool CachedChunkPacket::operator==(const CachedChunkPacket& other) const {
    if (this->mLongHashCode != other.mLongHashCode) return false;
    return mBigHash == other.mBigHash;
}

} // namespace playback::io
