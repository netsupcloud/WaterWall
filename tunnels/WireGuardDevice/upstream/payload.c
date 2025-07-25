#include "structure.h"

#include "loggers/network_logger.h"



static wireguard_peer_t *peerLookupByAllowedIp(wireguard_device_t *device, const ip_addr_t *ipaddr)
{
    wireguard_peer_t *result = NULL;
    wireguard_peer_t *tmp;
    int x, y;
    for (x = 0; (!result) && (x < WIREGUARD_MAX_PEERS); x++)
    {
        tmp = &device->peers[x];
        if (tmp->valid)
        {
            for (y = 0; y < WIREGUARD_MAX_SRC_IPS; y++)
            {
                if (tmp->allowed_source_ips[y].valid)
                {
                    if ((ipaddr->type == IPADDR_TYPE_V4) &&
                        (tmp->allowed_source_ips[y].ip.type == IPADDR_TYPE_V4))
                    {
                        if (ip4AddrNetcmp(ip_2_ip4(ipaddr),
                                           ip_2_ip4(&tmp->allowed_source_ips[y].ip),
                                           ip_2_ip4(&tmp->allowed_source_ips[y].mask)))
                        {
                            result = tmp;
                            break;
                        }
                    }
                    else if ((ipaddr->type == IPADDR_TYPE_V6) &&
                             (tmp->allowed_source_ips[y].ip.type == IPADDR_TYPE_V6))
                    {
                        if (ip6AddrNetcmp(ip_2_ip6(ipaddr),
                                           ip_2_ip6(&tmp->allowed_source_ips[y].ip),
                                           ip_2_ip6(&tmp->allowed_source_ips[y].mask)))
                        {
                            result = tmp;
                            break;
                        }
                    }
                }
            }
        }
    }
    return result;
}

err_t wireguardifOutputToPeer(wireguard_device_t *device, sbuf_t *q, const ip_addr_t *ipaddr, wireguard_peer_t *peer)
{
    assert(q);
    discard ipaddr;

    // The LWIP IP layer wants to send an IP packet out over the interface - we need to encrypt and send it to the peer
    message_transport_data_t *hdr;
    err_t                     result;
    uint32_t                  unpadded_len;
    uint32_t                  padded_len;
    uint32_t                  header_len = 16;
    uint8_t                  *dst;
    uint32_t                  now;
    wireguard_keypair_t      *keypair = &peer->curr_keypair;

    // Note: We may not be able to use the current keypair if we haven't received data, may need to resort to using
    // previous keypair
    if (keypair->valid && (! keypair->initiator) && (keypair->last_rx == 0))
    {
        keypair = &peer->prev_keypair;
    }

    if (keypair->valid && (keypair->initiator || keypair->last_rx != 0))
    {

        if (! wireguardExpired(keypair->keypair_millis, REJECT_AFTER_TIME) &&
            (keypair->sending_counter < REJECT_AFTER_MESSAGES))
        {

            // Calculate the outgoing packet size - round up to next 16 bytes, add 16 bytes for header
            if (sbufGetLength(q) > 0)
            {
                // This is actual transport data
                unpadded_len = sbufGetLength(q);
            }
            else
            {
                // This is a keep-alive
                unpadded_len = 0;
            }
            padded_len = (unpadded_len + 15) & 0xFFFFFFF0; // Round up to next 16 byte boundary
            assert(padded_len + WIREGUARD_AUTHTAG_LEN <= 1516);
            assert(padded_len + WIREGUARD_AUTHTAG_LEN <= SMALL_BUFFER_SIZE);
            sbufSetLength(q, padded_len + WIREGUARD_AUTHTAG_LEN); // 1500 is the max packet size which is divided by 16

            // The buffer needs to be allocated from "transport" pool to leave room for LwIP generated IP headers
            // The IP packet consists of 16 byte header (struct message_transport_data), data padded upto 16 byte
            // boundary + encrypted auth tag (16 bytes)
            // buf = pbufAlloc(sbuf_tRANSPORT, header_len + padded_len + WIREGUARD_AUTHTAG_LEN, PBUF_RAM);
            sbufShiftLeft(q, header_len);
            sbufWriteZeros(q, header_len);

            // Note: allocating buf from RAM above guarantees that the buf is in one section and not chained
            // - i.e payload points to the contiguous memory region
            // memorySet(buf->payload, 0, buf->tot_len);

            hdr = (message_transport_data_t *) sbufGetMutablePtr(q);

            hdr->type     = MESSAGE_TRANSPORT_DATA;
            hdr->receiver = keypair->remote_index;
            // Alignment required... pbuf_alloc has probably aligned data, but want to be sure
            U64TO8_LITTLE(hdr->counter, keypair->sending_counter);

            // Copy the encrypted (padded) data to the output packet - chacha20poly1305Encrypt() can encrypt data
            // in-place which avoids call to mem_malloc
            dst = &hdr->enc_packet[0];
            // if ((padded_len > 0) && q)
            // {
            //     // Note: before copying make sure we have inserted the IP header checksum
            //     // The IP header checksum (and other checksums in the IP packet - e.g. ICMP) need to be calculated
            //     // by LWIP before calling The Wireguard interface always needs checksums to be generated in software
            //     // but the base ts may have some checksums generated by hardware

            //     // Copy buf to memory - handles case where buf is chained
            //     pbufCopyPartial(q, dst, unpadded_len, 0);
            // }

            // Then encrypt
            wireguardEncryptPacket(dst, dst, padded_len, keypair);

            result = wireguardifPeerOutput(device, q, peer);
            q      = NULL; // buffer is consumed by wireguardifPeerOutput

            if (result == ERR_OK)
            {
                now              = getTickMS();
                peer->last_tx    = now;
                keypair->last_tx = now;
            }

            // Check to see if we should rekey
            if (keypair->sending_counter >= REKEY_AFTER_MESSAGES)
            {
                peer->send_handshake = true;
            }
            else if (keypair->initiator && wireguardExpired(keypair->keypair_millis, REKEY_AFTER_TIME))
            {
                peer->send_handshake = true;
            }
        }
        else
        {
            // key has expired...
            LOGD("WrireugardDevice: DISCARDING PACKET - KEY EXPIRED");
            keypairDestroy(keypair);
            result = ERR_CONN;
        }
    }
    else
    {
        LOGD("WrireugardDevice: DISCARDING PACKET - NO VALID KEYS");
        // No valid keys!
        result = ERR_CONN;
    }
    if (q != NULL)
    {
        bufferpoolReuseBuffer(getWorkerBufferPool(getWID()), q);
    }
    return result;
}

// This is used as the output function for the Wireguard INTERFACE
// The ipaddr here is the one inside the VPN which we use to lookup the correct peer/endpoint
static err_t wireguardifOutput(wireguard_device_t *device, sbuf_t *q, const ip_addr_t *ipaddr)
{
    // Send to peer that matches dest IP
    wireguard_peer_t *peer = peerLookupByAllowedIp(device, ipaddr);
    if (peer)
    {
        return wireguardifOutputToPeer(device, q, ipaddr, peer);
    }
    return ERR_RTE;
}

void wireguarddeviceTunnelUpStreamPayload(tunnel_t *t, line_t *l, sbuf_t *buf)
{
    discard l;

    if (sbufGetLength(buf) < sizeof(ip4_hdr_t))
    {
        bufferpoolReuseBuffer(getWorkerBufferPool(getWID()), buf);
        return;
    }
    wgd_tstate_t       *state = tunnelGetState(t);
    wireguard_device_t *dev   = tunnelGetState(t);
    uint8_t            *data  = sbufGetMutablePtr(buf);

    mutexLock(&state->mutex);
    state->locked = true;

    wireguard_peer_t *peer = NULL;
    ip_addr_t dest;

    if (IP_HDR_GET_VERSION(data) == 4)
    {
        ip4_hdr_t *header = (ip4_hdr_t *) data;
        ipAddrCopyFromIp4(dest, header->dest);
        peer = peerLookupByAllowedIp(dev, &dest);
    }
    else if (IP_HDR_GET_VERSION(data) == 6)
    {
        ip6_hdr_t *header = (ip6_hdr_t *) data;
        ip6_addr_t dest_ip6;
        ip6AddrCopyFromPacket(dest_ip6, header->dest);
        ipAddrCopyFromIp6(dest, dest_ip6);
        peer = peerLookupByAllowedIp(dev, &dest);
    }

    if(peer)
    {
        wireguardifOutputToPeer(dev, buf, &dest, peer);
    }
    else
    {
        LOGD("WireguardDevice cannot route a packet");
        bufferpoolReuseBuffer(getWorkerBufferPool(getWID()), buf);
    }

    if (state->locked)
    {
        state->locked = false;
        mutexUnlock(&state->mutex);
    
    }
}
