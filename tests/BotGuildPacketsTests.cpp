/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#include "BotGuildPackets.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

// Standalone packet tests: only the core ByteBuffer header is required.
ByteBufferPositionException::ByteBufferPositionException(bool, size_t, size_t, size_t) {}
ByteBufferSourceException::ByteBufferSourceException(size_t, size_t, size_t) {}
namespace Skyfire
{
    void Assert(char const*, int, char const*, char const*) { std::abort(); }
}

// Fixture follows WorldSession::SendPetitionShowSignatures (5.4.8).
static ByteBuffer OfferPacket(uint64 charter, uint64 owner, std::vector<uint64> const& signedBy)
{
    ByteBuffer data;
    ObjectGuid p(charter), o(owner);
    data.WriteGuidMask(o, 1);
    data.WriteGuidMask(p, 3);
    data.WriteGuidMask(o, 3);
    data.WriteGuidMask(p, 4, 0);
    data.WriteGuidMask(o, 7, 5);
    data.WriteGuidMask(p, 1, 5, 7);
    data.WriteGuidMask(o, 0, 6);
    data.WriteGuidMask(p, 6);
    data.WriteGuidMask(o, 2, 4);
    data.WriteBits(signedBy.size(), 21);
    for (uint64 signer : signedBy)
        data.WriteGuidMask(ObjectGuid(signer), 2, 0, 4, 7, 5, 1, 6, 3);
    data.WriteGuidMask(p, 2);
    data.FlushBits();
    for (uint64 signer : signedBy)
    {
        data.WriteGuidBytes(ObjectGuid(signer), 6, 0, 1, 3, 2, 5, 7, 4);
        data << uint32(0);
    }
    data.WriteGuidBytes(p, 6, 5, 4);
    data.WriteGuidBytes(o, 4);
    data.WriteGuidBytes(p, 1);
    data << uint32(charter);
    data.WriteGuidBytes(p, 2, 3, 7);
    data.WriteGuidBytes(o, 5, 6, 3, 7, 1, 0);
    data.WriteGuidBytes(p, 0);
    data.WriteGuidBytes(o, 2);
    return data;
}

int main()
{
    BotGuildPackets::Offer offer;
    // Sparse and full GUIDs exercise zero-byte masks and XOR byte encoding.
    for (uint64 charter : { uint64(0x4000000000000001), uint64(0x4000123456789abc) })
        for (uint64 owner : { uint64(1), uint64(0x0102030405060708) })
            for (std::vector<uint64> signers : { std::vector<uint64>{}, { 7, 8, 9 }, { 7, 42, 9 } })
            {
                ByteBuffer packet = OfferPacket(charter, owner, signers);
                assert(BotGuildPackets::ReadOffer(packet, 42, offer));
                assert(offer.petition == charter && offer.owner == owner);
                assert(offer.alreadySigned == (signers.size() && signers[1] == 42));
                assert(packet.rpos() == 0); // observer must not consume source
                for (size_t length = 0; length < packet.size(); ++length)
                {
                    ByteBuffer truncated;
                    if (length)
                        truncated.append(packet.contents(), length);
                    assert(!BotGuildPackets::ReadOffer(truncated, 42, offer));
                }
                assert(!BotGuildPackets::ReadOffer(packet, owner, offer));
                packet << uint8(0);
                assert(!BotGuildPackets::ReadOffer(packet, 42, offer));

                ByteBuffer sign;
                BotGuildPackets::WriteSignature(sign, charter);
                // Decode exactly as HandlePetitionSignOpcode does.
                assert(sign.read<uint8>() == 0);
                ObjectGuid guid;
                sign.ReadGuidMask(guid, 4, 2, 0, 1, 5, 3, 6, 7);
                sign.ReadGuidBytes(guid, 6, 1, 7, 2, 5, 3, 0, 4);
                assert(uint64(guid) == charter && sign.rpos() == sign.size());
            }
    assert(!BotGuildPackets::ReadOffer(OfferPacket(1, 2, std::vector<uint64>(256, 3)), 42, offer));
    assert(!BotGuildPackets::ReadOffer(OfferPacket(0, 2, {}), 42, offer));
    assert(!BotGuildPackets::ReadOffer(OfferPacket(1, 0, {}), 42, offer));
    std::cout << "Bot guild packet tests passed\n";
}
