#include "structure.h"

#include "loggers/network_logger.h"

void halfduplexserverTunnelUpStreamPayload(tunnel_t *t, line_t *l, sbuf_t *buf)
{
    halfduplexserver_tstate_t *ts = tunnelGetState(t);
    halfduplexserver_lstate_t *ls = lineGetState(l, t);

    switch (ls->state)
    {

    case kCsUnkown: {

        if (ls->buffering)
        {
            buf = sbufConcat(ls->buffering, buf);

            ls->buffering = NULL;
        }

        if (sbufGetLength(buf) < sizeof(uint64_t))
        {
            ls->buffering = buf;
            buf           = NULL;
            return;
        }
        const bool is_upload                    = (((uint8_t *) sbufGetRawPtr(buf))[0] & kHLFDCmdDownload) == 0x0;
        ((uint8_t *) sbufGetMutablePtr(buf))[0] = (((uint8_t *) sbufGetRawPtr(buf))[0] & kHLFDCmdUpload);

        hash_t hash = 0x0;
        sbufReadUnAlignedUI64(buf, (uint64_t *) &hash);
        ls->hash = hash;

        if (is_upload)
        {
            ls->upload_line = l;
            mutexLock(&(ts->download_line_map_mutex));
            hmap_cons_t_iter f_iter = hmap_cons_t_find(&(ts->download_line_map), hash);
            bool             found  = f_iter.ref != hmap_cons_t_end(&(ts->download_line_map)).ref;

            if (found)
            {
                // pair is found
                uint8_t tid_download_line = lineGetWID((*f_iter.ref).second->download_line);

                if (tid_download_line == getWID())
                {
                    line_t *download_line = ((halfduplexserver_lstate_t *) ((*f_iter.ref).second))->download_line;
                    ls->download_line     = download_line;

                    halfduplexserver_lstate_t *download_line_ls =
                        ((halfduplexserver_lstate_t *) ((*f_iter.ref).second));

                    hmap_cons_t_erase_at(&(ts->download_line_map), f_iter);
                    mutexUnlock(&(ts->download_line_map_mutex));
                    ls->state = kCsUploadDirect;

                    assert(download_line_ls->state == kCsDownloadInTable);

                    download_line_ls->state       = kCsDownloadDirect;
                    download_line_ls->upload_line = l;

                    line_t *main_line =
                        lineCreate(tunnelchainGetLinePool(tunnelGetChain(t), lineGetWID(l)), lineGetWID(l));
                    download_line_ls->main_line = main_line;
                    ls->main_line               = main_line;

                    halfduplexserver_lstate_t *ls_mainline = lineGetState(main_line, t);
                    halfduplexserverLinestateInitialize(ls_mainline);

                    ls_mainline->upload_line   = l;
                    ls_mainline->download_line = download_line;
                    ls_mainline->main_line     = main_line;

                    lineLock(main_line);
                    tunnelNextUpStreamInit(t, main_line);

                    if (! lineIsAlive(main_line))
                    {
                        bufferpoolReuseBuffer(lineGetBufferPool(l), buf);
                        lineUnlock(main_line);
                        return;
                    }
                    lineUnlock(main_line);
                    sbufShiftRight(buf, sizeof(uint64_t));
                    if (sbufGetLength(buf) > 0)
                    {
                        tunnelNextUpStreamPayload(t, main_line, buf);
                        return;
                    }
                    bufferpoolReuseBuffer(lineGetBufferPool(l), buf);
                }
                else
                {
                    mutexUnlock(&(ts->download_line_map_mutex));

                    halfduplexserverLinestateDestroy(ls);
                    pipeTo(t, l, tid_download_line);
                    tunnel_t *prev_tun = t->prev;
                    // wirte to pipe
                    tunnelNextUpStreamPayload(prev_tun, l, buf);
                    return; // piped to another worker which has waiting connections
                }
            }
            else
            {
                mutexUnlock(&(ts->download_line_map_mutex));
                ls->state = kCsUploadInTable;

                mutexLock(&(ts->upload_line_map_mutex));
                bool push_succeed = hmap_cons_t_insert(&(ts->upload_line_map), hash, ls).inserted;
                mutexUnlock(&(ts->upload_line_map_mutex));

                if (! push_succeed)
                {
                    LOGW("HalfDuplexServer: duplicate upload connection closed");
                    halfduplexserverLinestateDestroy(ls);
                    bufferpoolReuseBuffer(lineGetBufferPool(l), buf);

                    tunnelPrevDownStreamFinish(t, l);
                    return;
                }

                ls->buffering = buf;
                buf           = NULL;
                // upload connection is waiting in the pool
            }
        }
        else
        {
            ls->download_line = l;

            mutexLock(&(ts->upload_line_map_mutex));
            hmap_cons_t_iter f_iter = hmap_cons_t_find(&(ts->upload_line_map), hash);
            bool             found  = f_iter.ref != hmap_cons_t_end(&(ts->upload_line_map)).ref;

            if (found)
            {
                // pair is found
                wid_t wid_upload_line = lineGetWID((*f_iter.ref).second->upload_line);

                if (wid_upload_line == getWID())
                {
                    bufferpoolReuseBuffer(lineGetBufferPool(l), buf);

                    halfduplexserver_lstate_t *upload_line_ls = ((halfduplexserver_lstate_t *) ((*f_iter.ref).second));
                    hmap_cons_t_erase_at(&(ts->upload_line_map), f_iter);
                    mutexUnlock(&(ts->upload_line_map_mutex));
                    ls->state       = kCsDownloadDirect;
                    ls->upload_line = upload_line_ls->upload_line;

                    assert(upload_line_ls->state == kCsUploadInTable);

                    upload_line_ls->state         = kCsUploadDirect;
                    upload_line_ls->download_line = l;

                    line_t *main_line =
                        lineCreate(tunnelchainGetLinePool(tunnelGetChain(t), lineGetWID(l)), lineGetWID(l));

                    upload_line_ls->main_line = main_line;
                    ls->main_line             = main_line;

                    halfduplexserver_lstate_t *ls_mainline = lineGetState(main_line, t);
                    halfduplexserverLinestateInitialize(ls_mainline);

                    ls_mainline->upload_line   = ls->upload_line;
                    ls_mainline->download_line = l;
                    ls_mainline->main_line     = main_line;

                    lineLock(main_line);
                    tunnelNextUpStreamInit(t, main_line);

                    if (! lineIsAlive(main_line))
                    {
                        lineUnlock(main_line);

                        return;
                    }
                    lineUnlock(main_line);

                    assert(upload_line_ls->buffering);

                    if (sbufGetLength(upload_line_ls->buffering) > 0)
                    {
                        sbufShiftRight(upload_line_ls->buffering, sizeof(uint64_t));
                        tunnelNextUpStreamPayload(t, main_line, upload_line_ls->buffering);

                        upload_line_ls->buffering = NULL;
                    }
                    else
                    {
                        bufferpoolReuseBuffer(lineGetBufferPool(l), upload_line_ls->buffering);
                        upload_line_ls->buffering = NULL;
                    }
                }
                else
                {
                    mutexUnlock(&(ts->upload_line_map_mutex));

                    halfduplexserverLinestateDestroy(ls);
                    pipeTo(t, l, wid_upload_line);
                    tunnel_t *prev_tun = t->prev;
                    // wirte to pipe
                    tunnelNextUpStreamPayload(prev_tun, l, buf);
                    return; // piped to another worker which has waiting connections
                }
            }
            else
            {
                mutexUnlock(&(ts->upload_line_map_mutex));
                bufferpoolReuseBuffer(lineGetBufferPool(l), buf);

                ls->state = kCsDownloadInTable;

                mutexLock(&(ts->download_line_map_mutex));
                bool push_succeed = hmap_cons_t_insert(&(ts->download_line_map), hash, ls).inserted;
                mutexUnlock(&(ts->download_line_map_mutex));
                if (! push_succeed)
                {
                    LOGW("HalfDuplexServer: duplicate download connection closed");
                    halfduplexserverLinestateDestroy(ls);

                    tunnelPrevDownStreamFinish(t, l);
                    return;
                }
            }
        }

        break;
    }
    break;

    case kCsUploadInTable:
        if (ls->buffering)
        {
            ls->buffering = sbufConcat(ls->buffering, buf);
        }
        else
        {
            ls->buffering = buf;
        }
        if (sbufGetLength(ls->buffering) >= kMaxBuffering)
        {
            bufferpoolReuseBuffer(lineGetBufferPool(l), ls->buffering);
            ls->buffering = NULL;
        }
        break;

    case kCsUploadDirect:
        if (LIKELY(ls->main_line != NULL))
        {
            // on asyc closeing download line, for a very low chance 
            tunnelNextUpStreamPayload(t, ls->main_line, buf);
        }
        else
        {
            bufferpoolReuseBuffer(lineGetBufferPool(l), buf);
        }
        break;

    case kCsDownloadDirect:
    case kCsDownloadInTable:
        bufferpoolReuseBuffer(lineGetBufferPool(l), ls->buffering);
        break;
    }
}
