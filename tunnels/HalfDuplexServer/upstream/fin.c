#include "structure.h"

#include "loggers/network_logger.h"

static void localAsyncCloseLine(worker_t *worker, void *arg1, void *arg2, void *arg3)
{

    discard worker;
    discard arg3;

    tunnel_t                  *t  = arg1;
    line_t                    *l  = arg2;
    halfduplexserver_lstate_t *ls = lineGetState(l, t);

    halfduplexserverLinestateDestroy(ls);
    tunnelPrevDownStreamFinish(t, l);
    lineUnlock(l);
}

void halfduplexserverTunnelUpStreamFinish(tunnel_t *t, line_t *l)
{
    halfduplexserver_tstate_t *ts = tunnelGetState(t);
    halfduplexserver_lstate_t *ls = lineGetState(l, t);

    switch (ls->state)
    {

    case kCsUnkown:
        if (ls->buffering)
        {
            bufferpoolReuseBuffer(lineGetBufferPool(l), ls->buffering);
        }
        halfduplexserverLinestateDestroy(ls);
        break;

    case kCsUploadInTable: {

        mutexLock(&(ts->upload_line_map_mutex));

        hmap_cons_t_iter f_iter = hmap_cons_t_find(&(ts->upload_line_map), ls->hash);
        bool             found  = f_iter.ref != hmap_cons_t_end(&(ts->upload_line_map)).ref;
        if (! found)
        {
            LOGF("HalfDuplexServer: Thread safety is done incorrectly  [%s:%d]", __FILENAME__, __LINE__);
            exit(1);
        }
        hmap_cons_t_erase_at(&(ts->upload_line_map), f_iter);

        mutexUnlock(&(ts->upload_line_map_mutex));
        bufferpoolReuseBuffer(lineGetBufferPool(l), ls->buffering);
        halfduplexserverLinestateDestroy(ls);
    }
    break;

    case kCsDownloadInTable: {
        mutexLock(&(ts->download_line_map_mutex));

        hmap_cons_t_iter f_iter = hmap_cons_t_find(&(ts->download_line_map), ls->hash);
        bool             found  = f_iter.ref != hmap_cons_t_end(&(ts->download_line_map)).ref;
        if (! found)
        {
            LOGF("HalfDuplexServer: Thread safety is done incorrectly  [%s:%d]", __FILENAME__, __LINE__);
            exit(1);
        }
        hmap_cons_t_erase_at(&(ts->download_line_map), f_iter);

        mutexUnlock(&(ts->download_line_map_mutex));
        halfduplexserverLinestateDestroy(ls);
    }
    break;

    case kCsDownloadDirect: {
        halfduplexserver_lstate_t *ls_download_line = ls;
        //  halfduplexserver_lstate_t *ls_upload_line = ls;
        //  halfduplexserver_lstate_t *ls_main_line = ls;

        ls_download_line->download_line = NULL;

        if (ls_download_line->main_line)
        {
            halfduplexserver_lstate_t *ls_main_line = ls;
            tunnelNextUpStreamFinish(t, ls_download_line->main_line);

            halfduplexserverLinestateDestroy(ls_main_line);
            lineDestroy(ls_download_line->main_line);
            ls_download_line->main_line = NULL;
        }

        line_t *upload_line = ls_download_line->upload_line;
        if (upload_line)
        {
            halfduplexserver_lstate_t *ls_upload_line = lineGetState(upload_line, t);
            ls_upload_line->main_line                 = NULL;
            ls_upload_line->download_line             = NULL;

            lineLock(upload_line);
            sendWorkerMessageForceQueue(lineGetWID(upload_line), localAsyncCloseLine, t, upload_line, NULL);

        }

        halfduplexserverLinestateDestroy(ls_download_line);
    }
    break;

    case kCsUploadDirect: {
        halfduplexserver_lstate_t *ls_upload_line = ls;

        ls_upload_line->upload_line = NULL;

        if (ls_upload_line->main_line)
        {
            halfduplexserver_lstate_t *ls_main_line = ls;
            tunnelNextUpStreamFinish(t, ls_upload_line->main_line);

            halfduplexserverLinestateDestroy(ls_main_line);
            lineDestroy(ls_upload_line->main_line);
            ls_upload_line->main_line = NULL;
        }
        line_t *download_line = ls_upload_line->upload_line;

        if (download_line)
        {
            halfduplexserver_lstate_t *ls_download_line = lineGetState(download_line, t);
            ls_download_line->main_line                 = NULL;
            ls_download_line->upload_line               = NULL;

            lineLock(download_line);
            sendWorkerMessageForceQueue(lineGetWID(download_line), localAsyncCloseLine, t, download_line, NULL);
        }
 

        halfduplexserverLinestateDestroy(ls_upload_line);

    }
    break;

    default:
        LOGF("HalfDuplexServer: Unexpected  [%s:%d]", __FILENAME__, __LINE__);
        exit(1);
        break;
    }
}

