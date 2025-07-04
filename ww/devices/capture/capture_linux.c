#include "capture.h"
#include "generic_pool.h"
#include "global_state.h"
#include "loggers/internal_logger.h"
#include "wchan.h"
#include "worker.h"
#include "wproc.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_queue.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>

enum
{
    kReadPacketSize                       = 1500,
    kEthDataLen                           = 1500,
    kMasterMessagePoolsbufGetLeftCapacity = 64,
    kQueueLen                             = 512,
    kCaptureWriteChannelQueueMax          = 128
};

static const char *ip_tables_enable_queue_mi  = "iptables -I INPUT -s %s -j NFQUEUE --queue-num %d";
static const char *ip_tables_disable_queue_mi = "iptables -D INPUT -s %s -j NFQUEUE --queue-num %d";

struct msg_event
{
    capture_device_t *cdev;
    sbuf_t           *buf;
};

static pool_item_t *allocCaptureMsgPoolHandle(master_pool_t *pool, void *userdata)
{
    discard userdata;
    discard pool;
    return memoryAllocate(sizeof(struct msg_event));
}

static void destroyCaptureMsgPoolHandle(master_pool_t *pool, master_pool_item_t *item, void *userdata)
{
    discard pool;
    discard userdata;
    memoryFree(item);
}

static void localThreadEventReceived(wevent_t *ev)
{
    struct msg_event *msg = weventGetUserdata(ev);
    wid_t             tid = (wid_t) (wloopGetWid(weventGetLoop(ev)));

    atomicSubExplicit(&(msg->cdev->packets_queued), 1, memory_order_release);

    msg->cdev->read_event_callback(msg->cdev, msg->cdev->userdata, msg->buf, tid);

    masterpoolReuseItems(msg->cdev->reader_message_pool, (void **) &msg, 1, msg->cdev);
}

static void distributePacketPayload(capture_device_t *cdev, wid_t target_wid, sbuf_t *buf)
{
    atomicAddExplicit(&(cdev->packets_queued), 1, memory_order_release);

    struct msg_event *msg;
    masterpoolGetItems(cdev->reader_message_pool, (const void **) &(msg), 1, cdev);

    *msg = (struct msg_event) {.cdev = cdev, .buf = buf};

    wevent_t ev;
    memorySet(&ev, 0, sizeof(ev));
    ev.loop = getWorkerLoop(target_wid);
    ev.cb   = localThreadEventReceived;
    weventSetUserData(&ev, msg);
    wloopPostEvent(getWorkerLoop(target_wid), &ev);
}

/*
 * Send a message to the netfilter system and wait for an acknowledgement.
 */
static bool netfilterSendMessage(int netfilter_socket, uint16_t nl_type, int nfa_type, uint16_t res_id, bool ack,
                                 void *msg, size_t size)
{
    size_t  nl_size = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct nfgenmsg))) + NFA_ALIGN(NFA_LENGTH(size));
    uint8_t buff[nl_size];
    memorySet(buff, 0, nl_size);
    struct nlmsghdr *nl_hdr = (struct nlmsghdr *) buff;

    nl_hdr->nlmsg_len   = NLMSG_LENGTH(sizeof(struct nfgenmsg));
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | (ack ? NLM_F_ACK : 0);
    nl_hdr->nlmsg_type  = (NFNL_SUBSYS_QUEUE << 8) | nl_type;
    nl_hdr->nlmsg_pid   = 0;
    nl_hdr->nlmsg_seq   = 0;

    struct nfgenmsg *nl_gen_msg = (struct nfgenmsg *) (nl_hdr + 1);
    nl_gen_msg->version         = NFNETLINK_V0;
    nl_gen_msg->nfgen_family    = AF_UNSPEC;
    nl_gen_msg->res_id          = htons(res_id);

    struct nfattr *nl_attr     = (struct nfattr *) (buff + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    size_t         nl_attr_len = NFA_LENGTH(size);
    nl_hdr->nlmsg_len          = NLMSG_ALIGN(nl_hdr->nlmsg_len) + NFA_ALIGN(nl_attr_len);
    nl_attr->nfa_type          = nfa_type;
    nl_attr->nfa_len           = NFA_LENGTH(size);

    memoryMove(NFA_DATA(nl_attr), msg, size);

    struct sockaddr_nl nl_addr;
    memorySet(&nl_addr, 0x0, sizeof(nl_addr));
    nl_addr.nl_family = AF_NETLINK;

    if (sendto(netfilter_socket, buff, sizeof(buff), 0, (struct sockaddr *) &nl_addr, sizeof(nl_addr)) !=
        (long) sizeof(buff))
    {
        return false;
    }

    if (! ack)
    {
        return true;
    }

    uint8_t   ack_buff[64];
    socklen_t nl_addr_len = sizeof(nl_addr);
    ssize_t   result =
        recvfrom(netfilter_socket, ack_buff, sizeof(ack_buff), 0, (struct sockaddr *) &nl_addr, &nl_addr_len);
    nl_hdr = (struct nlmsghdr *) ack_buff;

    if (result < 0)
    {
        return false;
    }

    if (nl_addr_len != sizeof(nl_addr) || nl_addr.nl_pid != 0)
    {
        errno = EINVAL;
        return false;
    }

    if (NLMSG_OK(nl_hdr, result) && nl_hdr->nlmsg_type == NLMSG_ERROR)
    {
        errno = -(*(int *) NLMSG_DATA(nl_hdr));
        return (errno == 0);
    }

    errno = EBADMSG;
    return false;
}

/*
 * Set a netfilter configuration option.
 */
static bool netfilterSetConfig(int netfilter_socket, uint8_t cmd, uint16_t qnum, uint16_t pf)
{
    struct nfqnl_msg_config_cmd nl_cmd = {.command = cmd, .pf = htons(pf)};
    return netfilterSendMessage(netfilter_socket, NFQNL_MSG_CONFIG, NFQA_CFG_CMD, qnum, true, &nl_cmd, sizeof(nl_cmd));
}

/*
 * Set the netfilter parameters.
 */
static bool netfilterSetParams(int netfilter_socket, uint16_t qnumber, uint8_t mode, uint32_t range)
{
    struct nfqnl_msg_config_params nl_params = {.copy_mode = mode, .copy_range = htonl(range)};
    return netfilterSendMessage(netfilter_socket, NFQNL_MSG_CONFIG, NFQA_CFG_PARAMS, qnumber, true, &nl_params,
                                sizeof(nl_params));
}

/*
 * Set the netfilter queue length.
 */
static bool netfilterSetQueueLength(int netfilter_socket, uint16_t qnumber, uint32_t qlen)
{
    return netfilterSendMessage(netfilter_socket, NFQNL_MSG_CONFIG, NFQA_CFG_QUEUE_MAXLEN, qnumber, true, &qlen,
                                sizeof(qlen));
}

/*
 * Get a packet from netfilter.
 */
static int netfilterGetPacket(int netfilter_socket, uint16_t qnumber, sbuf_t *buff)
{
    // Read a message from netlink
    char               nl_buff[512 + kEthDataLen + sizeof(struct ethhdr) + sizeof(struct nfqnl_msg_packet_hdr)];
    struct sockaddr_nl nl_addr;
    socklen_t          nl_addr_len = sizeof(nl_addr);
    ssize_t            result =
        recvfrom(netfilter_socket, nl_buff, sizeof(nl_buff), 0, (struct sockaddr *) &nl_addr, &nl_addr_len);

    if (result <= (int) sizeof(struct nlmsghdr))
    {
        errno = EINVAL;
        return -1;
    }
    if (nl_addr_len != sizeof(nl_addr) || nl_addr.nl_pid != 0)
    {
        errno = EINVAL;
        return false;
    }

    struct nlmsghdr *nl_hdr = (struct nlmsghdr *) nl_buff;
    if (NFNL_SUBSYS_ID(nl_hdr->nlmsg_type) != NFNL_SUBSYS_QUEUE)
    {
        errno = EINVAL;
        return -1;
    }
    if (NFNL_MSG_TYPE(nl_hdr->nlmsg_type) != NFQNL_MSG_PACKET)
    {
        errno = EINVAL;
        return -1;
    }
    if (nl_hdr->nlmsg_len < sizeof(struct nfgenmsg))
    {
        errno = EINVAL;
        return -1;
    }

    // Get the packet data
    int nl_size0 = NLMSG_SPACE(sizeof(struct nfgenmsg));
    if ((int) nl_hdr->nlmsg_len < nl_size0)
    {
        errno = EINVAL;
        return -1;
    }
    struct nfattr               *nl_attr      = NFM_NFA(NLMSG_DATA(nl_hdr));
    int                          nl_attr_size = (int) (nl_hdr->nlmsg_len - NLMSG_ALIGN(nl_size0));
    bool                         found_data = false, found_pkt_hdr = false;
    uint8_t                     *nl_data      = NULL;
    size_t                       nl_data_size = 0;
    struct nfqnl_msg_packet_hdr *nl_pkt_hdr   = NULL;
    while (NFA_OK(nl_attr, nl_attr_size))
    {
        int nl_attr_type = NFA_TYPE(nl_attr);
        switch (nl_attr_type)
        {
        case NFQA_PAYLOAD:
            if (found_data)
            {
                errno = EINVAL;
                return -1;
            }
            found_data   = true;
            nl_data      = (uint8_t *) NFA_DATA(nl_attr);
            nl_data_size = (size_t) NFA_PAYLOAD(nl_attr);
            break;
        case NFQA_PACKET_HDR:
            if (found_pkt_hdr)
            {
                errno = EINVAL;
                return -1;
            }
            found_pkt_hdr = true;
            nl_pkt_hdr    = (struct nfqnl_msg_packet_hdr *) NFA_DATA(nl_attr);
            break;
        }
        nl_attr = NFA_NEXT(nl_attr, nl_attr_size);
    }
    if (! found_data || ! found_pkt_hdr)
    {
        errno = EINVAL;
        return -1;
    }

    // Tell netlink to drop the packet
    struct nfqnl_msg_verdict_hdr nl_verdict;
    nl_verdict.verdict = htonl(NF_DROP);
    nl_verdict.id      = nl_pkt_hdr->packet_id;
    if (! netfilterSendMessage(netfilter_socket, NFQNL_MSG_VERDICT, NFQA_VERDICT_HDR, qnumber, false, &nl_verdict,
                               sizeof(nl_verdict)))
    {
        return -1;
    }

    // Copy the packet's contents to the output buffer.
    // Also add a phony ethernet header.
    sbufSetLength(buff, nl_data_size);
    // struct ethhdr *eth_header = (struct ethhdr *) buff;
    // memorySet(&eth_header->h_dest, 0x0, ETH_ALEN);
    // memorySet(&eth_header->h_source, 0x0, ETH_ALEN);
    // eth_header->h_proto = htons(ETH_P_IP);

    struct iphdr *ip_header = (struct iphdr *) sbufGetMutablePtr(buff);
    memoryMove(ip_header, nl_data, nl_data_size);

    return (int) (nl_data_size);
}

static WTHREAD_ROUTINE(routineReadFromCapture) // NOLINT
{
    capture_device_t *cdev = userdata;
    sbuf_t           *buf;
    ssize_t           nread;

    struct pollfd fds[2];
    fds[0].fd     = cdev->handle;
#if defined (OS_OPENBSD)
    fds[0].events = POLLIN;
#else
    fds[0].events = POLL_IN;
#endif
    fds[1].fd     = cdev->linux_pipe_fds[0];
#if defined (OS_OPENBSD)
    fds[1].events = POLLIN;
#else
    fds[1].events = POLL_IN;
#endif

    while (atomicLoadExplicit(&(cdev->running), memory_order_relaxed))
    {
        if (atomicLoadExplicit(&(cdev->packets_queued), memory_order_acquire) > 256)
        {
            ww_msleep(1);
            continue;
        }
        buf = bufferpoolGetSmallBuffer(cdev->reader_buffer_pool);

        buf     = sbufReserveSpace(buf, kReadPacketSize);
        int ret = poll(fds, 2, -1);
        if (ret > 0)
        {
            if (fds[1].revents & POLLIN)
            {
                bufferpoolReuseBuffer(cdev->reader_buffer_pool, buf);

                LOGW("RawDevice: Exit read routine due to pipe event");
                break;
            }
            if (fds[0].revents & POLLIN)
            {
                nread = netfilterGetPacket(cdev->handle, cdev->queue_number, buf);

                if (nread == 0)
                {
                    bufferpoolReuseBuffer(cdev->reader_buffer_pool, buf);
                    LOGW("CaptureDevice: Exit read routine due to End Of File");
                    return 0;
                }

                if (nread < 0)
                {
                    bufferpoolReuseBuffer(cdev->reader_buffer_pool, buf);
                    LOGW("CaptureDevice: failed to read a packet from netfilter socket, retrying...");
                    continue;
                }

                sbufSetLength(buf, nread);

                distributePacketPayload(cdev, getNextDistributionWID(), buf);
            }
        }
    }

    return 0;
}

// static WTHREAD_ROUTINE(routineWriteToCapture) // NOLINT
// {
//     capture_device_t *cdev = userdata;
//     sbuf_t           *buf;
//     ssize_t           nwrite;

//     while (atomicLoadExplicit(&(cdev->running), memory_order_relaxed))
//     {
//         if (! chanRecv(cdev->writer_buffer_channel, (void **) &buf))
//         {
//             LOGD("CaptureDevice: routine write will exit due to channel closed");
//             return 0;
//         }

//         struct iphdr *ip_header = (struct iphdr *) sbufGetRawPtr(buf);

//         struct sockaddr_in to_addr = {.sin_family = AF_INET, .sin_addr.s_addr = ip_header->daddr};

//         nwrite =
//             sendto(cdev->handle, ip_header, sbufGetLength(buf), 0, (struct sockaddr *) (&to_addr), sizeof(to_addr));

//         bufferpoolReuseBuffer(cdev->writer_buffer_pool, buf);

//         if (nwrite == 0)
//         {
//             LOGW("CaptureDevice: Exit write routine due to End Of File");
//             return 0;
//         }

//         if (nwrite < 0)
//         {
//             LOGW("CaptureDevice: writing a packet to Capture device failed, code: %d", (int) nwrite);
//             if (errno == EINVAL || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
//             {
//                 continue;
//             }
//             LOGE("CaptureDevice: Exit write routine due to critical error");
//             return 0;
//         }
//     }
//     return 0;
// }

bool caputredeviceWrite(capture_device_t *cdev, sbuf_t *buf)
{
    discard cdev;
    discard buf;
    return false;
    //     bool closed = false;
    //     if (! chanTrySend(cdev->writer_buffer_channel, &buf, &closed))
    //     {
    //         if (closed)
    //         {
    //             LOGE("CaptureDevice: write failed, channel was closed");
    //         }
    //         else
    //         {
    //             LOGE("CaptureDevice:write failed, ring is full");
    //         }
    //         return false;
    //     }
    //     return true;
}

bool caputredeviceBringUp(capture_device_t *cdev)
{
    assert(! cdev->up);

    if (execCmd(cdev->bringup_command).exit_code != 0)
    {
        LOGE("CaptureDevicer: command failed: %s", cdev->bringup_command);
        terminateProgram(1);
        return false;
    }
    bufferpoolUpdateAllocationPaddings(cdev->writer_buffer_pool,
                                       bufferpoolGetLargeBufferPadding(getWorkerBufferPool(getWID())),
                                       bufferpoolGetSmallBufferPadding(getWorkerBufferPool(getWID())));

    bufferpoolUpdateAllocationPaddings(cdev->reader_buffer_pool,
                                       bufferpoolGetLargeBufferPadding(getWorkerBufferPool(getWID())),
                                       bufferpoolGetSmallBufferPadding(getWorkerBufferPool(getWID())));

    cdev->up      = true;
    cdev->running = true;

    LOGD("CaptureDevice: device %s is now up", cdev->name);

    cdev->read_thread = threadCreate(cdev->routine_reader, cdev);
    return true;
}

bool caputredeviceBringDown(capture_device_t *cdev)
{
    assert(cdev->up);

    cdev->running = false;
    cdev->up      = false;

    atomicThreadFence(memory_order_release);

    if (execCmd(cdev->bringdown_command).exit_code != 0)
    {
        LOGE("CaptureDevicer: command failed: %s", cdev->bringdown_command);
        terminateProgram(1);
    }
    if (cdev->read_event_callback != NULL)
    {
        ssize_t _unused = write(cdev->linux_pipe_fds[1], "x", 1);
        (void) _unused;
        threadJoin(cdev->read_thread);
    }

    threadJoin(cdev->read_thread);
    threadJoin(cdev->write_thread);
    LOGD("CaptureDevice: device %s is now down", cdev->name);

    return true;
}

capture_device_t *caputredeviceCreate(const char *name, const char *capture_ip, void *userdata,
                                      CaptureReadEventHandle cb)
{

    int socket_netfilter = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (socket_netfilter < 0)
    {
        LOGE("CaptureDevice: unable to create a netfilter socket");
        return NULL;
    }

    struct sockaddr_nl nl_addr;
    memorySet(&nl_addr, 0x0, sizeof(nl_addr));
    nl_addr.nl_family = AF_NETLINK;
    nl_addr.nl_pid    = 0;

    if (bind(socket_netfilter, (struct sockaddr *) &nl_addr, sizeof(nl_addr)) != 0)
    {
        LOGE("CaptureDevice: unable to bind netfilter socket to current process");
        close(socket_netfilter);
        return NULL;
    }

    if (! netfilterSetConfig(socket_netfilter, NFQNL_CFG_CMD_PF_UNBIND, 0, PF_INET))
    {
        LOGE("CaptureDevice: unable to unbind netfilter from PF_INET");
        close(socket_netfilter);
        return NULL;
    }
    if (! netfilterSetConfig(socket_netfilter, NFQNL_CFG_CMD_PF_BIND, 0, PF_INET))
    {
        LOGE("CaptureDevice: unable to bind netfilter to PF_INET");
        close(socket_netfilter);
        return NULL;
    }
    int queue_number = GSTATE.capturedevice_queue_start_number++;

    char *bringup_cmd   = memoryAllocate(100);
    char *bringdown_cmd = memoryAllocate(100);
    stringNPrintf(bringup_cmd, 100, ip_tables_enable_queue_mi, capture_ip, queue_number);
    stringNPrintf(bringdown_cmd, 100, ip_tables_disable_queue_mi, capture_ip, queue_number);

    if (! netfilterSetConfig(socket_netfilter, NFQNL_CFG_CMD_BIND, queue_number, 0))
    {
        LOGE("CaptureDevice: unable to bind netfilter to queue number %u", queue_number);
        close(socket_netfilter);
        return NULL;
    }

    uint32_t range = kEthDataLen + sizeof(struct ethhdr) + sizeof(struct nfqnl_msg_packet_hdr);
    if (! netfilterSetParams(socket_netfilter, queue_number, NFQNL_COPY_PACKET, range))
    {
        LOGE("CaptureDevice: unable to set netfilter into copy packet mode with maximum "
             "buffer size %u",
             range);

        close(socket_netfilter);
        return NULL;
    }
    if (! netfilterSetQueueLength(socket_netfilter, queue_number, kQueueLen))
    {
        LOGE("CaptureDevice: unable to set netfilter queue maximum length to %u", kQueueLen);

        close(socket_netfilter);
        return NULL;
    }

    buffer_pool_t *reader_bpool =
        bufferpoolCreate(GSTATE.masterpool_buffer_pools_large, GSTATE.masterpool_buffer_pools_small, RAM_PROFILE,
                         bufferpoolGetLargeBufferSize(getWorkerBufferPool(getWID())),
                         bufferpoolGetSmallBufferSize(getWorkerBufferPool(getWID()))

        );

    buffer_pool_t *writer_bpool =
        bufferpoolCreate(GSTATE.masterpool_buffer_pools_large, GSTATE.masterpool_buffer_pools_small, RAM_PROFILE,
                         bufferpoolGetLargeBufferSize(getWorkerBufferPool(getWID())),
                         bufferpoolGetSmallBufferSize(getWorkerBufferPool(getWID()))

        );

    capture_device_t *cdev = memoryAllocate(sizeof(capture_device_t));

    *cdev =
        (capture_device_t) {.name                = stringDuplicate(name),
                            .running             = false,
                            .up                  = false,
                            .routine_reader      = routineReadFromCapture,
                            .routine_writer      = NULL,
                            .handle              = socket_netfilter,
                            .queue_number        = queue_number,
                            .read_event_callback = cb,
                            .userdata            = userdata,
                            .reader_message_pool = masterpoolCreateWithCapacity(kMasterMessagePoolsbufGetLeftCapacity),
                            .packets_queued      = 0,
                            .netfilter_queue_number = queue_number,
                            .bringup_command        = bringup_cmd,
                            .bringdown_command      = bringdown_cmd,
                            .reader_buffer_pool     = reader_bpool,
                            .writer_buffer_pool     = writer_bpool};
    if (pipe(cdev->linux_pipe_fds) != 0)
    {
        LOGE("CaptureDevice: failed to create pipe for linux_pipe_fds");
        memoryFree(cdev->name);
        memoryFree(cdev->bringup_command);
        memoryFree(cdev->bringdown_command);
        bufferpoolDestroy(cdev->reader_buffer_pool);
        bufferpoolDestroy(cdev->writer_buffer_pool);
        masterpoolDestroy(cdev->reader_message_pool);
        close(cdev->handle);
        memoryFree(cdev);
        return NULL;
    }

    masterpoolInstallCallBacks(cdev->reader_message_pool, allocCaptureMsgPoolHandle, destroyCaptureMsgPoolHandle);

    return cdev;
}

void capturedeviceDestroy(capture_device_t *cdev)
{
    if (cdev->up)
    {
        caputredeviceBringDown(cdev);
    }
    memoryFree(cdev->name);
    memoryFree(cdev->bringup_command);
    memoryFree(cdev->bringdown_command);
    bufferpoolDestroy(cdev->reader_buffer_pool);
    bufferpoolDestroy(cdev->writer_buffer_pool);
    masterpoolMakeEmpty(cdev->reader_message_pool,NULL);
    masterpoolDestroy(cdev->reader_message_pool);
    close(cdev->handle);
    memoryFree(cdev);
}
