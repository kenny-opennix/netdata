#define NETDATA_RRD_INTERNALS 1
#include "common.h"

RRDHOST *localhost = NULL;

pthread_rwlock_t rrd_rwlock = PTHREAD_RWLOCK_INITIALIZER;

time_t rrdhost_free_orphan_time = 3600;

// ----------------------------------------------------------------------------
// RRDHOST index

int rrdhost_compare(void* a, void* b) {
    if(((RRDHOST *)a)->hash_machine_guid < ((RRDHOST *)b)->hash_machine_guid) return -1;
    else if(((RRDHOST *)a)->hash_machine_guid > ((RRDHOST *)b)->hash_machine_guid) return 1;
    else return strcmp(((RRDHOST *)a)->machine_guid, ((RRDHOST *)b)->machine_guid);
}

avl_tree_lock rrdhost_root_index = {
        .avl_tree = { NULL, rrdhost_compare },
        .rwlock = AVL_LOCK_INITIALIZER
};

RRDHOST *rrdhost_find_guid(const char *guid, uint32_t hash) {
    debug(D_RRDHOST, "Searching in index for host with guid '%s'", guid);

    RRDHOST tmp;
    strncpyz(tmp.machine_guid, guid, GUID_LEN);
    tmp.hash_machine_guid = (hash)?hash:simple_hash(tmp.machine_guid);

    return (RRDHOST *)avl_search_lock(&(rrdhost_root_index), (avl *) &tmp);
}

#define rrdhost_index_add(rrdhost) (RRDHOST *)avl_insert_lock(&(rrdhost_root_index), (avl *)(rrdhost))
#define rrdhost_index_del(rrdhost) (RRDHOST *)avl_remove_lock(&(rrdhost_root_index), (avl *)(rrdhost))


// ----------------------------------------------------------------------------
// RRDHOST - internal helpers

static inline void rrdhost_init_hostname(RRDHOST *host, const char *hostname) {
    freez(host->hostname);
    host->hostname = strdupz(hostname);
    host->hash_hostname = simple_hash(host->hostname);
}

static inline void rrdhost_init_os(RRDHOST *host, const char *os) {
    freez(host->os);
    host->os = strdupz(os?os:"unknown");
}

static inline void rrdhost_init_machine_guid(RRDHOST *host, const char *machine_guid) {
    strncpy(host->machine_guid, machine_guid, GUID_LEN);
    host->machine_guid[GUID_LEN] = '\0';
    host->hash_machine_guid = simple_hash(host->machine_guid);
}


// ----------------------------------------------------------------------------
// RRDHOST - add a host

RRDHOST *rrdhost_create(const char *hostname,
        const char *guid,
        const char *os,
        int update_every,
        int entries,
        RRD_MEMORY_MODE memory_mode,
        int health_enabled,
        int rrdpush_enabled,
        char *rrdpush_destination,
        char *rrdpush_api_key,
        int is_localhost
) {

    debug(D_RRDHOST, "Host '%s': adding with guid '%s'", hostname, guid);

    RRDHOST *host = callocz(1, sizeof(RRDHOST));

    host->rrd_update_every    = update_every;
    host->rrd_history_entries = entries;
    host->rrd_memory_mode     = memory_mode;
    host->health_enabled      = (memory_mode == RRD_MEMORY_MODE_NONE)? 0 : health_enabled;
    host->rrdpush_enabled     = (rrdpush_enabled && rrdpush_destination && *rrdpush_destination && rrdpush_api_key && *rrdpush_api_key);
    host->rrdpush_destination = (host->rrdpush_enabled)?strdupz(rrdpush_destination):NULL;
    host->rrdpush_api_key     = (host->rrdpush_enabled)?strdupz(rrdpush_api_key):NULL;

    host->rrdpush_pipe[0] = -1;
    host->rrdpush_pipe[1] = -1;
    host->rrdpush_socket  = -1;

    pthread_mutex_init(&host->rrdpush_mutex, NULL);
    pthread_rwlock_init(&host->rrdhost_rwlock, NULL);

    rrdhost_init_hostname(host, hostname);
    rrdhost_init_machine_guid(host, guid);
    rrdhost_init_os(host, os);

    avl_init_lock(&(host->rrdset_root_index),      rrdset_compare);
    avl_init_lock(&(host->rrdset_root_index_name), rrdset_compare_name);
    avl_init_lock(&(host->rrdfamily_root_index),   rrdfamily_compare);
    avl_init_lock(&(host->variables_root_index),   rrdvar_compare);

    // ------------------------------------------------------------------------
    // initialize health variables

    host->health_log.next_log_id = 1;
    host->health_log.next_alarm_id = 1;
    host->health_log.max = 1000;
    host->health_log.next_log_id =
    host->health_log.next_alarm_id = (uint32_t)now_realtime_sec();

    long n = config_get_number(CONFIG_SECTION_HEALTH, "in memory max health log entries", host->health_log.max);
    if(n < 10) {
        error("Host '%s': health configuration has invalid max log entries %ld. Using default %u", host->hostname, n, host->health_log.max);
        config_set_number(CONFIG_SECTION_HEALTH, "in memory max health log entries", (long)host->health_log.max);
    }
    else
        host->health_log.max = (unsigned int)n;

    pthread_rwlock_init(&(host->health_log.alarm_log_rwlock), NULL);

    char filename[FILENAME_MAX + 1];

    if(is_localhost) {

        host->cache_dir  = strdupz(netdata_configured_cache_dir);
        host->varlib_dir = strdupz(netdata_configured_varlib_dir);

    }
    else {
        // this is not localhost - append our GUID to localhost path

        snprintfz(filename, FILENAME_MAX, "%s/%s", netdata_configured_cache_dir, host->machine_guid);
        host->cache_dir = strdupz(filename);

        if(host->rrd_memory_mode == RRD_MEMORY_MODE_MAP || host->rrd_memory_mode == RRD_MEMORY_MODE_SAVE) {
            int r = mkdir(host->cache_dir, 0775);
            if(r != 0 && errno != EEXIST)
                error("Host '%s': cannot create directory '%s'", host->hostname, host->cache_dir);
        }

        snprintfz(filename, FILENAME_MAX, "%s/%s", netdata_configured_varlib_dir, host->machine_guid);
        host->varlib_dir = strdupz(filename);

        if(host->health_enabled) {
            int r = mkdir(host->varlib_dir, 0775);
            if(r != 0 && errno != EEXIST)
                error("Host '%s': cannot create directory '%s'", host->hostname, host->varlib_dir);

            snprintfz(filename, FILENAME_MAX, "%s/health", host->varlib_dir);
            r = mkdir(filename, 0775);
            if(r != 0 && errno != EEXIST)
                error("Host '%s': cannot create directory '%s'", host->hostname, filename);
        }

    }

    snprintfz(filename, FILENAME_MAX, "%s/health/health-log.db", host->varlib_dir);
    host->health_log_filename = strdupz(filename);

    snprintfz(filename, FILENAME_MAX, "%s/alarm-notify.sh", netdata_configured_plugins_dir);
    host->health_default_exec = strdupz(config_get(CONFIG_SECTION_HEALTH, "script to execute on alarm", filename));
    host->health_default_recipient = strdup("root");


    // ------------------------------------------------------------------------
    // load health configuration

    if(host->health_enabled) {
        health_alarm_log_load(host);
        health_alarm_log_open(host);

        rrdhost_wrlock(host);
        health_readdir(host, health_config_dir());
        rrdhost_unlock(host);
    }


    // ------------------------------------------------------------------------
    // link it and add it to the index

    rrd_wrlock();

    if(is_localhost) {
        host->next = localhost;
        localhost = host;
    }
    else {
        if(localhost) {
            host->next = localhost->next;
            localhost->next = host;
        }
        else localhost = host;
    }

    RRDHOST *t = rrdhost_index_add(host);

    if(t != host) {
        error("Host '%s': cannot add host with machine guid '%s' to index. It already exists as host '%s' with machine guid '%s'.", host->hostname, host->machine_guid, t->hostname, t->machine_guid);
        rrdhost_free(host);
        host = NULL;
    }
    else {
        info("Host '%s' with guid '%s' initialized"
                     ", os %s"
                     ", update every %d"
                     ", memory mode %s"
                     ", history entries %d"
                     ", streaming %s"
                     " (to '%s' with api key '%s')"
                     ", health %s"
                     ", cache_dir '%s'"
                     ", varlib_dir '%s'"
                     ", health_log '%s'"
                     ", alarms default handler '%s'"
                     ", alarms default recipient '%s'"
             , host->hostname
             , host->machine_guid
             , host->os
             , host->rrd_update_every
             , rrd_memory_mode_name(host->rrd_memory_mode)
             , host->rrd_history_entries
             , host->rrdpush_enabled?"enabled":"disabled"
             , host->rrdpush_destination?host->rrdpush_destination:""
             , host->rrdpush_api_key?host->rrdpush_api_key:""
             , host->health_enabled?"enabled":"disabled"
             , host->cache_dir
             , host->varlib_dir
             , host->health_log_filename
             , host->health_default_exec
             , host->health_default_recipient
        );
    }

    rrd_unlock();

    return host;
}

RRDHOST *rrdhost_find_or_create(
          const char *hostname
        , const char *guid
        , const char *os
        , int update_every
        , int history
        , RRD_MEMORY_MODE mode
        , int health_enabled
        , int rrdpush_enabled
        , char *rrdpush_destination
        , char *rrdpush_api_key
) {
    debug(D_RRDHOST, "Searching for host '%s' with guid '%s'", hostname, guid);

    RRDHOST *host = rrdhost_find_guid(guid, 0);
    if(!host) {
        host = rrdhost_create(
                hostname
                , guid
                , os
                , update_every
                , history
                , mode
                , health_enabled
                , rrdpush_enabled
                , rrdpush_destination
                , rrdpush_api_key
                , 0
        );
    }
    else {
        host->health_enabled = health_enabled;

        if(strcmp(host->hostname, hostname)) {
            char *t = host->hostname;
            char *n = strdupz(hostname);
            host->hostname = n;
            freez(t);
        }

        if(host->rrd_update_every != update_every)
            error("Host '%s' has an update frequency of %d seconds, but the wanted one is %d seconds.", host->hostname, host->rrd_update_every, update_every);

        if(host->rrd_history_entries != history)
            error("Host '%s' has history of %d entries, but the wanted one is %d entries.", host->hostname, host->rrd_history_entries, history);

        if(host->rrd_memory_mode != mode)
            error("Host '%s' has memory mode '%s', but the wanted one is '%s'.", host->hostname, rrd_memory_mode_name(host->rrd_memory_mode), rrd_memory_mode_name(mode));
    }

    rrdhost_cleanup_remote_stale(host);

    return host;
}

void rrdhost_cleanup_remote_stale(RRDHOST *protected) {
    rrd_wrlock();

    RRDHOST *h;
    rrdhost_foreach_write(h) {
        if(h != protected
           && h != localhost
           && !h->connected_senders
           && h->senders_disconnected_time + rrdhost_free_orphan_time > now_realtime_sec()) {
            info("Host '%s' with machine guid '%s' is obsolete - cleaning up.", h->hostname, h->machine_guid);
            rrdhost_save(h);
            rrdhost_free(h);
            break;
        }
    }

    rrd_unlock();
}

// ----------------------------------------------------------------------------
// RRDHOST global / startup initialization

void rrd_init(char *hostname) {
    health_init();
    registry_init();
    rrdpush_init();

    debug(D_RRDHOST, "Initializing localhost with hostname '%s'", hostname);
    localhost = rrdhost_create(
            hostname
            , registry_get_this_machine_guid()
            , os_type
            , default_rrd_update_every
            , default_rrd_history_entries
            , default_rrd_memory_mode
            , default_health_enabled
            , default_rrdpush_enabled
            , default_rrdpush_destination
            , default_rrdpush_api_key
            , 1
    );
}

// ----------------------------------------------------------------------------
// RRDHOST - lock validations
// there are only used when NETDATA_INTERNAL_CHECKS is set

void rrdhost_check_rdlock_int(RRDHOST *host, const char *file, const char *function, const unsigned long line) {
    debug(D_RRDHOST, "Checking read lock on host '%s'", host->hostname);

    int ret = pthread_rwlock_trywrlock(&host->rrdhost_rwlock);
    if(ret == 0)
        fatal("RRDHOST '%s' should be read-locked, but it is not, at function %s() at line %lu of file '%s'", host->hostname, function, line, file);
}

void rrdhost_check_wrlock_int(RRDHOST *host, const char *file, const char *function, const unsigned long line) {
    debug(D_RRDHOST, "Checking write lock on host '%s'", host->hostname);

    int ret = pthread_rwlock_tryrdlock(&host->rrdhost_rwlock);
    if(ret == 0)
        fatal("RRDHOST '%s' should be write-locked, but it is not, at function %s() at line %lu of file '%s'", host->hostname, function, line, file);
}

void rrd_check_rdlock_int(const char *file, const char *function, const unsigned long line) {
    debug(D_RRDHOST, "Checking read lock on all RRDs");

    int ret = pthread_rwlock_trywrlock(&rrd_rwlock);
    if(ret == 0)
        fatal("RRDs should be read-locked, but it are not, at function %s() at line %lu of file '%s'", function, line, file);
}

void rrd_check_wrlock_int(const char *file, const char *function, const unsigned long line) {
    debug(D_RRDHOST, "Checking write lock on all RRDs");

    int ret = pthread_rwlock_tryrdlock(&rrd_rwlock);
    if(ret == 0)
        fatal("RRDs should be write-locked, but it are not, at function %s() at line %lu of file '%s'", function, line, file);
}

// ----------------------------------------------------------------------------
// RRDHOST - free

void rrdhost_free(RRDHOST *host) {
    if(!host) return;

    info("Freeing all memory for host '%s'...", host->hostname);

    rrd_check_wrlock();     // make sure the RRDs are write locked
    rrdhost_wrlock(host);   // lock this RRDHOST

    // ------------------------------------------------------------------------
    // release its children resources

    while(host->rrdset_root) rrdset_free(host->rrdset_root);

    while(host->alarms) rrdcalc_free(host, host->alarms);
    while(host->templates) rrdcalctemplate_free(host, host->templates);
    health_alarm_log_free(host);


    // ------------------------------------------------------------------------
    // remove it from the indexes

    if(rrdhost_index_del(host) != host)
        error("RRDHOST '%s' removed from index, deleted the wrong entry.", host->hostname);


    // ------------------------------------------------------------------------
    // unlink it from the host

    if(host == localhost) {
        localhost = host->next;
    }
    else {
        // find the previous one
        RRDHOST *h;
        for(h = localhost; h && h->next != host ; h = h->next) ;

        // bypass it
        if(h) h->next = host->next;
        else error("Request to free RRDHOST '%s': cannot find it", host->hostname);
    }

    // ------------------------------------------------------------------------
    // free it

    rrdpush_sender_thread_stop(host);

    freez(host->os);
    freez(host->cache_dir);
    freez(host->varlib_dir);
    freez(host->rrdpush_api_key);
    freez(host->rrdpush_destination);
    freez(host->health_default_exec);
    freez(host->health_default_recipient);
    freez(host->health_log_filename);
    freez(host->hostname);
    rrdhost_unlock(host);
    freez(host);

    info("Host memory cleanup completed...");
}

void rrdhost_free_all(void) {
    rrd_wrlock();
    while(localhost) rrdhost_free(localhost);
    rrd_unlock();
}

// ----------------------------------------------------------------------------
// RRDHOST - save

void rrdhost_save(RRDHOST *host) {
    if(!host) return;

    info("Saving host '%s' database...", host->hostname);

    RRDSET *st;
    RRDDIM *rd;

    // we get a write lock
    // to ensure only one thread is saving the database
    rrdhost_wrlock(host);

    rrdset_foreach_write(st, host) {
        rrdset_rdlock(st);

        if(st->rrd_memory_mode == RRD_MEMORY_MODE_SAVE) {
            debug(D_RRD_STATS, "Saving stats '%s' to '%s'.", st->name, st->cache_filename);
            savememory(st->cache_filename, st, st->memsize);
        }

        rrddim_foreach_read(rd, st) {
            if(likely(rd->rrd_memory_mode == RRD_MEMORY_MODE_SAVE)) {
                debug(D_RRD_STATS, "Saving dimension '%s' to '%s'.", rd->name, rd->cache_filename);
                savememory(rd->cache_filename, rd, rd->memsize);
            }
        }

        rrdset_unlock(st);
    }

    rrdhost_unlock(host);
}

void rrdhost_save_all(void) {
    info("Saving database...");

    rrd_rdlock();

    RRDHOST *host;
    rrdhost_foreach_read(host)
        rrdhost_save(host);

    rrd_unlock();
}
