#include "global_state.h"
#include "buffer_pool.h"
#include "loggers/core_logger.h"
#include "loggers/dns_logger.h"
#include "loggers/internal_logger.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "managers/signal_manager.h"
#include "managers/socket_manager.h"

ww_global_state_t global_ww_state = {0};

ww_global_state_t *globalStateGet(void)
{
    return &GSTATE;
}


void globalStateSet(struct ww_global_state_s *state)
{
    assert(! GSTATE.initialized && state->initialized);
    GSTATE = *state;

    setCoreLogger(GSTATE.core_logger);
    setNetworkLogger(GSTATE.network_logger);
    setDnsLogger(GSTATE.dns_logger);
    setInternalLogger(GSTATE.ww_logger);
    setSignalManager(GSTATE.signal_manager);
    socketmanagerSet(GSTATE.socekt_manager);
    nodemanagerSetState(GSTATE.node_manager);
}

static void initializeShortCuts(void)
{
    assert(GSTATE.initialized);

    static const int kShourtcutsCount = 5;
    const int        total_workers    = WORKERS_COUNT;

    void **space = (void **) memoryAllocate(sizeof(void *) * kShourtcutsCount * total_workers);

    GSTATE.shortcut_loops              = (wloop_t **) (space + (0ULL * total_workers));
    GSTATE.shortcut_buffer_pools       = (buffer_pool_t **) (space + (1ULL * total_workers));
    GSTATE.shortcut_context_pools      = (generic_pool_t **) (space + (2ULL * total_workers));
    GSTATE.shortcut_pipetunnel_msg_pools = (generic_pool_t **) (space + (3ULL * total_workers));

    for (unsigned int tid = 0; tid < GSTATE.workers_count; tid++)
    {

        GSTATE.shortcut_context_pools[tid]      = WORKERS[tid].context_pool;
        GSTATE.shortcut_pipetunnel_msg_pools[tid] = WORKERS[tid].pipetunnel_msg_pool;
        GSTATE.shortcut_buffer_pools[tid]       = WORKERS[tid].buffer_pool;
        GSTATE.shortcut_loops[tid]              = WORKERS[tid].loop;
    }
}

static void initializeMasterPools(void)
{
    assert(GSTATE.initialized);

    GSTATE.masterpool_buffer_pools_large = masterpoolCreateWithCapacity(2 * ((0) + GSTATE.ram_profile));
    GSTATE.masterpool_buffer_pools_small = masterpoolCreateWithCapacity(2 * ((0) + GSTATE.ram_profile));
    GSTATE.masterpool_context_pools      = masterpoolCreateWithCapacity(2 * ((16) + GSTATE.ram_profile));
    GSTATE.masterpool_pipetunnel_msg_pools = masterpoolCreateWithCapacity(2 * ((8) + GSTATE.ram_profile));
}

void createGlobalState(const ww_construction_data_t init_data)
{
    GSTATE.initialized = true;

    // [Section] loggers
    {
        GSTATE.ww_logger = createInternalLogger(init_data.internal_logger_data.log_file_path,
                                                init_data.internal_logger_data.log_console);
        setInternalLoggerLevelByStr(init_data.internal_logger_data.log_level);

        GSTATE.core_logger =
            createCoreLogger(init_data.core_logger_data.log_file_path, init_data.core_logger_data.log_console);

        stringUpperCase(init_data.core_logger_data.log_level);
        setCoreLoggerLevelByStr(init_data.core_logger_data.log_level);

        GSTATE.network_logger =
            createNetworkLogger(init_data.network_logger_data.log_file_path, init_data.network_logger_data.log_console);

        stringUpperCase(init_data.network_logger_data.log_level);
        setNetworkLoggerLevelByStr(init_data.network_logger_data.log_level);

        GSTATE.dns_logger =
            createDnsLogger(init_data.dns_logger_data.log_file_path, init_data.dns_logger_data.log_console);

        stringUpperCase(init_data.dns_logger_data.log_level);
        setDnsLoggerLevelByStr(init_data.dns_logger_data.log_level);
    }

    // workers and pools creation
    {
        WORKERS_COUNT      = init_data.workers_count;
        GSTATE.ram_profile = init_data.ram_profile;

        if (WORKERS_COUNT <= 0 || WORKERS_COUNT > (254))
        {
            LOGW("workers count was not in valid range, value: %u range:[1 - %d]\n", WORKERS_COUNT, (254));
            WORKERS_COUNT = (254);
        }

        WORKERS = (worker_t *) memoryAllocate(sizeof(worker_t) * (WORKERS_COUNT));

        initializeMasterPools();

        for (unsigned int i = 0; i < WORKERS_COUNT; ++i)
        {
            workerInit(getWorker(i), i);
        }

        initializeShortCuts();
    }

    GSTATE.signal_manager = createSignalManager();
    startSignalManager();

    GSTATE.socekt_manager = socketmanagerCreate();
    GSTATE.node_manager   = nodemanagerCreate();

    // Spawn all workers except main worker which is current thread
    {
        for (unsigned int i = 1; i < WORKERS_COUNT; ++i)
        {
            workerRunNewThread(&WORKERS[i]);
        }
    }
}

void runMainThread(void)
{
    assert(GSTATE.initialized);

    WORKERS[0].thread = (wthread_t) NULL;
    workerRun(getWorker(0));

    LOGF("Unexpected: main loop joined");

    for (size_t i = 1; i < WORKERS_COUNT; i++)
    {
        threadJoin(getWorker(i)->thread);
    }
    LOGF("Unexpected: other loops joined");
    exit(1);
}
