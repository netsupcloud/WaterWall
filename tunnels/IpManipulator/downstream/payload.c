#include "structure.h"

#include "loggers/network_logger.h"

void ipmanipulatorDownStreamPayload(tunnel_t *t, line_t *l, sbuf_t *buf)
{
    ipmanipulator_tstate_t *state    = tunnelGetState(t);
    struct ip_hdr          *ipheader = (struct ip_hdr *) sbufGetMutablePtr(buf);

    if (state->manip_swap_tcp != 0 && IPH_V(ipheader) == 4 && IPH_PROTO(ipheader) == state->manip_swap_tcp)
    {
        IPH_PROTO_SET(ipheader, IPPROTO_TCP);
        l->recalculate_checksum = true;
    }
    tunnelPrevDownStreamPayload(t, l, buf);
}
