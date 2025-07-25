#include "network_logger.h"

struct logger_s;

static logger_t *logger = NULL;

static void destroyNetworkLogger(void)
{
    if (logger)
    {
        loggerSyncFile(logger);
        loggerDestroy(logger);
        logger = NULL;
    }
}

static void networkLoggerHandleOnlyStdStream(int loglevel, const char *buf, int len)
{
    switch (loglevel)
    {
    case LOG_LEVEL_WARN:
    case LOG_LEVEL_ERROR:
    case LOG_LEVEL_FATAL:
        stderrLogger(loglevel, buf, len);
        break;
    default:
        stdoutLogger(loglevel, buf, len);
        break;
    }
}

static void networkLoggerHandleWithStdStream(int loglevel, const char *buf, int len)
{
    networkLoggerHandleOnlyStdStream(loglevel, buf, len);
    loggerWrite(logger, buf, len);
}


static void networkLoggerHandle(int loglevel, const char *buf, int len)
{
    discard loglevel;
    loggerWrite(logger, buf, len);
}

logger_t *getNetworkLogger(void)
{
    return logger;
}
void setNetworkLogger(logger_t *newlogger)
{
    assert(logger == NULL);
    logger = newlogger;
}

logger_t *createNetworkLogger(const char *log_file, bool console)
{
    assert(logger == NULL);
    logger = loggerCreate();
    bool path_accepted = loggerSetFile(logger, log_file);
    if (console)
    {
        if (path_accepted)
        {
            loggerSetHandler(logger, networkLoggerHandleWithStdStream);
        }
        else
        {

            loggerSetHandler(logger, networkLoggerHandleOnlyStdStream);
        }
    }
    else if (path_accepted)
    {
        loggerSetHandler(logger, networkLoggerHandle);
    }
    return logger;
}

logger_handler getNetworkLoggerHandle(void)
{
    return loggerGetHandle(logger);
}
