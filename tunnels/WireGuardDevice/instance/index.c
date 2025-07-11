#include "structure.h"

#include "loggers/network_logger.h"

void wireguarddeviceTunnelOnIndex(tunnel_t *t, tunnel_array_t *arr, uint16_t *index, uint16_t *mem_offset)
{
    // using tunnel / adapter default handle for this action
    discard t;
    discard arr;
    discard index;
    discard mem_offset;
    LOGF("This Function is disabled, using the default PacketTunnel instead");
    terminateProgram(1);
}
