/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#ifndef SKYFIRE_BOT_GUILD_PACKETS_H
#define SKYFIRE_BOT_GUILD_PACKETS_H

#include "ByteBuffer.h"

namespace BotGuildPackets
{
    struct Offer
    {
        uint64 petition = 0;
        uint64 owner = 0;
        bool alreadySigned = false;
    };

    // SMSG_PETITION_SHOW_SIGNATURES is also sent after a successful signature.
    // Decode signers so that notification never becomes another sign request.
    inline bool ReadOffer(ByteBuffer const& source, uint64 bot, Offer& offer)
    {
        offer = Offer{};
        if (source.size() > 4096)
            return false;
        ByteBuffer data;
        if (source.empty())
            return false;
        data.append(source.contents(), source.size());
        try
        {
            ObjectGuid owner, petition;
            data.ReadGuidMask(owner, 1);
            data.ReadGuidMask(petition, 3);
            data.ReadGuidMask(owner, 3);
            data.ReadGuidMask(petition, 4, 0);
            data.ReadGuidMask(owner, 7, 5);
            data.ReadGuidMask(petition, 1, 5, 7);
            data.ReadGuidMask(owner, 0, 6);
            data.ReadGuidMask(petition, 6);
            data.ReadGuidMask(owner, 2, 4);
            uint32 const count = data.ReadBits(21);
            if (count > 255) // core serializes a uint8 signature count
                return false;
            std::vector<ObjectGuid> signers(count);
            for (auto& signer : signers)
                data.ReadGuidMask(signer, 2, 0, 4, 7, 5, 1, 6, 3);
            data.ReadGuidMask(petition, 2);
            for (auto& signer : signers)
            {
                data.ReadGuidBytes(signer, 6, 0, 1, 3, 2, 5, 7, 4);
                data.read_skip<uint32>();
                if (uint64(signer) == bot)
                    offer.alreadySigned = true;
            }
            data.ReadGuidBytes(petition, 6, 5, 4);
            data.ReadGuidBytes(owner, 4);
            data.ReadGuidBytes(petition, 1);
            uint32 const petitionLow = data.read<uint32>();
            data.ReadGuidBytes(petition, 2, 3, 7);
            data.ReadGuidBytes(owner, 5, 6, 3, 7, 1, 0);
            data.ReadGuidBytes(petition, 0);
            data.ReadGuidBytes(owner, 2);
            offer.petition = uint64(petition);
            offer.owner = uint64(owner);
            return data.rpos() == data.size() && offer.petition && offer.owner &&
                uint32(offer.petition) == petitionLow && offer.owner != bot;
        }
        catch (ByteBufferException const&)
        {
            return false;
        }
    }

    inline void WriteSignature(ByteBuffer& data, uint64 petition)
    {
        ObjectGuid guid(petition);
        data << uint8(0);
        data.WriteGuidMask(guid, 4, 2, 0, 1, 5, 3, 6, 7);
        data.FlushBits();
        data.WriteGuidBytes(guid, 6, 1, 7, 2, 5, 3, 0, 4);
    }
}

#endif
