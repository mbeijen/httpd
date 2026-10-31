/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mod_cache.h"

#include "cache_util.h"
#include <ap_provider.h>

#include "test_char.h"

APLOG_USE_MODULE(cache);

/* -------------------------------------------------------------- */

extern APR_OPTIONAL_FN_TYPE(ap_cache_generate_key) *cache_generate_key;

extern module AP_MODULE_DECLARE_DATA cache_module;

/* Determine if "url" matches the hostname, scheme and port and path
 * in "filter". All but the path comparisons are case-insensitive.
 */
static int uri_meets_conditions(const apr_uri_t *filter, const apr_size_t pathlen,
                                const apr_uri_t *url, const char *path)
{
    /* Scheme, hostname port and local part. The filter URI and the
     * URI we test may have the following shapes:
     *   /<path>
     *   <scheme>[:://<hostname>[:<port>][/<path>]]
     * That is, if there is no scheme then there must be only the path,
     * and we check only the path; if there is a scheme, we check the
     * scheme for equality, and then if present we match the hostname,
     * and then if present match the port, and finally the path if any.
     *
     * Note that this means that "/<path>" only matches local paths,
     * and to match proxied paths one *must* specify the scheme.
     */

    /* Is the filter is just for a local path or a proxy URI? */
    if (!filter->scheme) {
        if (url->scheme || url->hostname) {
            return 0;
        }
    }
    else {
        /* The URI scheme must be present and identical except for case. */
        if (!url->scheme || ap_cstr_casecmp(filter->scheme, url->scheme)) {
            return 0;
        }

        /* If the filter hostname is null or empty it matches any hostname,
         * if it begins with a "*" it matches the _end_ of the URI hostname
         * excluding the "*", if it begins with a "." it matches the _end_
         * of the URI * hostname including the ".", otherwise it must match
         * the URI hostname exactly. */

        if (filter->hostname && filter->hostname[0]) {
            if (filter->hostname[0] == '.') {
                const size_t fhostlen = strlen(filter->hostname);
                const size_t uhostlen = url->hostname ? strlen(url->hostname) : 0;

                if (fhostlen > uhostlen
                    || (url->hostname
                        && strcasecmp(filter->hostname,
                                      url->hostname + uhostlen - fhostlen))) {
                    return 0;
                }
            }
            else if (filter->hostname[0] == '*') {
                const size_t fhostlen = strlen(filter->hostname + 1);
                const size_t uhostlen = url->hostname ? strlen(url->hostname) : 0;

                if (fhostlen > uhostlen
                    || (url->hostname
                        && strcasecmp(filter->hostname + 1,
                                      url->hostname + uhostlen - fhostlen))) {
                    return 0;
                }
            }
            else if (!url->hostname || strcasecmp(filter->hostname, url->hostname)) {
                return 0;
            }
        }

        /* If the filter port is empty it matches any URL port.
         * If the filter or URL port are missing, or the URL port is
         * empty, they default to the port for their scheme. */

        if (!(filter->port_str && !filter->port_str[0])) {
            /* NOTE:  ap_port_of_scheme will return 0 if given NULL input */
            const unsigned fport = filter->port_str ? filter->port
                    : apr_uri_port_of_scheme(filter->scheme);
            const unsigned uport = (url->port_str && url->port_str[0])
                    ? url->port : apr_uri_port_of_scheme(url->scheme);

            if (fport != uport) {
                return 0;
            }
        }
    }

    /* For HTTP caching purposes, an empty (NULL) path is equivalent to
     * a single "/" path. RFCs 3986/2396
     */
    if (!path) {
        if (*filter->path == '/' && pathlen == 1) {
            return 1;
        }
        else {
            return 0;
        }
    }

    /* Url has met all of the filter conditions so far, determine
     * if the paths match.
     */
    return !strncmp(filter->path, path, pathlen);
}

int cache_use_early_url(request_rec *r)
{
    cache_server_conf *conf;

    if (r->proxyreq == PROXYREQ_PROXY) {
        return 1;
    }

    conf = ap_get_module_config(r->server->module_config, &cache_module);
    if (conf->quick) {
        return 1;
    }

    return 0;
}

static cache_provider_list *get_provider(request_rec *r, struct cache_enable *ent,
        cache_provider_list *providers)
{
    /* Fetch from global config and add to the list. */
    cache_provider *provider;
    provider = ap_lookup_provider(CACHE_PROVIDER_GROUP, ent->type,
                                  "0");
    if (!provider) {
        /* Log an error! */
    }
    else {
        cache_provider_list *newp;
        newp = apr_pcalloc(r->pool, sizeof(cache_provider_list));
        newp->provider_name = ent->type;
        newp->provider = provider;

        if (!providers) {
            providers = newp;
        }
        else {
            cache_provider_list *last = providers;

            while (last->next) {
                if (last->provider == provider) {
                    return providers;
                }
                last = last->next;
            }
            if (last->provider == provider) {
                return providers;
            }
            last->next = newp;
        }
    }

    return providers;
}

cache_provider_list *cache_get_providers(request_rec *r,
                                         cache_server_conf *conf)
{
    cache_dir_conf *dconf = ap_get_module_config(r->per_dir_config, &cache_module);
    cache_provider_list *providers = NULL;
    const char *path;
    int i;

    /* per directory cache disable */
    if (dconf->disable) {
        return NULL;
    }

    path = cache_use_early_url(r) ? r->parsed_uri.path : r->uri;

    /* global cache disable */
    for (i = 0; i < conf->cachedisable->nelts; i++) {
        struct cache_disable *ent =
                               (struct cache_disable *)conf->cachedisable->elts;
        if (uri_meets_conditions(&ent[i].url, ent[i].pathlen,
                                 &r->parsed_uri, path)) {
            /* Stop searching now. */
            return NULL;
        }
    }

    /* loop through all the per directory cacheenable entries */
    for (i = 0; i < dconf->cacheenable->nelts; i++) {
        struct cache_enable *ent =
                                (struct cache_enable *)dconf->cacheenable->elts;
        providers = get_provider(r, &ent[i], providers);
    }

    /* loop through all the global cacheenable entries */
    for (i = 0; i < conf->cacheenable->nelts; i++) {
        struct cache_enable *ent =
                                (struct cache_enable *)conf->cacheenable->elts;
        if (uri_meets_conditions(&ent[i].url, ent[i].pathlen,
                                 &r->parsed_uri, path)) {
            providers = get_provider(r, &ent[i], providers);
        }
    }

    return providers;
}


/* do a HTTP/1.1 age calculation */
CACHE_DECLARE(apr_int64_t) ap_cache_current_age(cache_info *info,
                                                const apr_time_t age_value,
                                                apr_time_t now)
{
    apr_time_t apparent_age, corrected_received_age, response_delay,
               corrected_initial_age, resident_time, current_age,
               age_value_usec;

    age_value_usec = apr_time_from_sec(age_value);

    /* Perform an HTTP/1.1 age calculation. (RFC2616 13.2.3) */

    apparent_age = MAX(0, info->response_time - info->date);
    corrected_received_age = MAX(apparent_age, age_value_usec);
    response_delay = info->response_time - info->request_time;
    corrected_initial_age = corrected_received_age + response_delay;
    resident_time = now - info->response_time;
    current_age = corrected_initial_age + resident_time;

    if (current_age < 0) {
        current_age = 0;
    }

    return apr_time_sec(current_age);
}

/**
 * Try obtain a cache wide lock on the given cache key.
 *
 * If we return APR_SUCCESS, we obtained the lock, and we are clear to
 * proceed to the backend. If we return APR_EEXIST, then the lock is
 * already locked, someone else has gone to refresh the backend data
 * already, so we must return stale data with a warning in the mean
 * time. If we return anything else, then something has gone pear
 * shaped, and we allow the request through to the backend regardless.
 *
 * This lock is created from the request pool, meaning that should
 * something go wrong and the lock isn't deleted on return of the
 * request headers from the backend for whatever reason, at worst the
 * lock will be cleaned up when the request dies or finishes.
 *
 * If something goes truly bananas and the lock isn't deleted when the
 * request dies, the lock will be trashed when its max-age is reached,
 * or when a request arrives containing a Cache-Control: no-cache. At
 * no point is it possible for this lock to permanently deny access to
 * the backend.
 */
apr_status_t cache_try_lock(cache_server_conf *conf, cache_request_rec *cache,
        request_rec *r)
{
    apr_status_t status;
    const char *lockname;
    const char *path;
    char dir[5];
    apr_time_t now = apr_time_now();
    apr_finfo_t finfo;
    apr_file_t *lockfile;
    void *dummy;

    finfo.mtime = 0;

    if (!conf || !conf->lock || !conf->lockpath) {
        /* no locks configured, leave */
        return APR_SUCCESS;
    }

    /* lock already obtained earlier? if so, success */
    apr_pool_userdata_get(&dummy, CACHE_LOCKFILE_KEY, r->pool);
    if (dummy) {
        return APR_SUCCESS;
    }

    /* create the key if it doesn't exist */
    if (!cache->key) {
        cache_handle_t *h;
        /*
         * Try to use the key of a possible open but stale cache
         * entry if we have one.
         */
        if (cache->handle != NULL) {
            h = cache->handle;
        }
        else {
            h = cache->stale_handle;
        }
        if ((h != NULL) &&
            (h->cache_obj != NULL) &&
            (h->cache_obj->key != NULL)) {
            cache->key = apr_pstrdup(r->pool, h->cache_obj->key);
        }
        else {
            cache_generate_key(r, r->pool, &cache->key);
        }
    }

    /* create a hashed filename from the key, and save it for later */
    lockname = ap_cache_generate_name(r->pool, 0, 0, cache->key);

    /* lock files represent discrete just-went-stale URLs "in flight", so
     * we support a simple two level directory structure, more is overkill.
     */
    dir[0] = '/';
    dir[1] = lockname[0];
    dir[2] = '/';
    dir[3] = lockname[1];
    dir[4] = 0;

    /* make the directories */
    path = apr_pstrcat(r->pool, conf->lockpath, dir, NULL);
    if (APR_SUCCESS != (status = apr_dir_make_recursive(path,
            APR_UREAD|APR_UWRITE|APR_UEXECUTE, r->pool))) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r, APLOGNO(00778)
                "Could not create a cache lock directory: %s",
                path);
        return status;
    }
    lockname = apr_pstrcat(r->pool, path, "/", lockname, NULL);
    apr_pool_userdata_set(lockname, CACHE_LOCKNAME_KEY, NULL, r->pool);

    /* is an existing lock file too old? */
    status = apr_stat(&finfo, lockname,
                APR_FINFO_MTIME | APR_FINFO_NLINK, r->pool);
    if (!(APR_STATUS_IS_ENOENT(status)) && APR_SUCCESS != status) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r, APLOGNO(00779)
                "Could not stat a cache lock file: %s",
                lockname);
        return status;
    }
    if ((status == APR_SUCCESS) && (((now - finfo.mtime) > conf->lockmaxage)
                                  || (now < finfo.mtime))) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, status, r, APLOGNO(00780)
                "Cache lock file for '%s' too old, removing: %s",
                r->uri, lockname);
        apr_file_remove(lockname, r->pool);
    }

    /* try obtain a lock on the file */
    if (APR_SUCCESS == (status = apr_file_open(&lockfile, lockname,
            APR_WRITE | APR_CREATE | APR_EXCL | APR_DELONCLOSE,
            APR_UREAD | APR_UWRITE, r->pool))) {
        apr_pool_userdata_set(lockfile, CACHE_LOCKFILE_KEY, NULL, r->pool);
    }
    return status;

}

/**
 * Remove the cache lock, if present.
 *
 * First, try to close the file handle, whose delete-on-close should
 * kill the file. Otherwise, just delete the file by name.
 *
 * If no lock name has yet been calculated, do the calculation of the
 * lock name first before trying to delete the file.
 *
 * If an optional bucket brigade is passed, the lock will only be
 * removed if the bucket brigade contains an EOS bucket.
 */
apr_status_t cache_remove_lock(cache_server_conf *conf,
        cache_request_rec *cache, request_rec *r, apr_bucket_brigade *bb)
{
    void *dummy;
    const char *lockname;

    if (!conf || !conf->lock || !conf->lockpath) {
        /* no locks configured, leave */
        return APR_SUCCESS;
    }
    if (bb) {
        apr_bucket *e;
        int eos_found = 0;
        for (e = APR_BRIGADE_FIRST(bb);
             e != APR_BRIGADE_SENTINEL(bb);
             e = APR_BUCKET_NEXT(e))
        {
            if (APR_BUCKET_IS_EOS(e)) {
                eos_found = 1;
                break;
            }
        }
        if (!eos_found) {
            /* no eos found in brigade, don't delete anything just yet,
             * we are not done.
             */
            return APR_SUCCESS;
        }
    }
    apr_pool_userdata_get(&dummy, CACHE_LOCKFILE_KEY, r->pool);
    if (dummy) {
        return apr_file_close((apr_file_t *)dummy);
    }
    apr_pool_userdata_get(&dummy, CACHE_LOCKNAME_KEY, r->pool);
    lockname = (const char *)dummy;
    if (!lockname) {
        char dir[5];

        /* create the key if it doesn't exist */
        if (!cache->key) {
            cache_generate_key(r, r->pool, &cache->key);
        }

        /* create a hashed filename from the key, and save it for later */
        lockname = ap_cache_generate_name(r->pool, 0, 0, cache->key);

        /* lock files represent discrete just-went-stale URLs "in flight", so
         * we support a simple two level directory structure, more is overkill.
         */
        dir[0] = '/';
        dir[1] = lockname[0];
        dir[2] = '/';
        dir[3] = lockname[1];
        dir[4] = 0;

        lockname = apr_pstrcat(r->pool, conf->lockpath, dir, "/", lockname, NULL);
    }
    return apr_file_remove(lockname, r->pool);
}

int ap_cache_check_no_cache(cache_request_rec *cache, request_rec *r)
{

    cache_server_conf *conf =
      (cache_server_conf *)ap_get_module_config(r->server->module_config,
                                                &cache_module);

    /*
     * At this point, we may have data cached, but the request may have
     * specified that cached data may not be used in a response.
     *
     * This is covered under RFC2616 section 14.9.4 (Cache Revalidation and
     * Reload Controls).
     *
     * - RFC2616 14.9.4 End to end reload, Cache-Control: no-cache, or Pragma:
     * no-cache. The server MUST NOT use a cached copy when responding to such
     * a request.
     */

    /* This value comes from the client's initial request. */
    if (!cache->control_in.parsed) {
        const char *cc_req = cache_table_getm(r->pool, r->headers_in,
                "Cache-Control");
        const char *pragma = cache_table_getm(r->pool, r->headers_in, "Pragma");
        ap_cache_control(r, &cache->control_in, cc_req, pragma, r->headers_in);
    }

    if (cache->control_in.no_cache) {

        if (!conf->ignorecachecontrol) {
            return 0;
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, APLOGNO(02657)
                    "Incoming request is asking for an uncached version of "
                    "%s, but we have been configured to ignore it and serve "
                    "cached content anyway", r->unparsed_uri);
        }
    }

    return 1;
}

int ap_cache_check_no_store(cache_request_rec *cache, request_rec *r)
{

    cache_server_conf *conf =
      (cache_server_conf *)ap_get_module_config(r->server->module_config,
                                                &cache_module);

    /*
     * At this point, we may have data cached, but the request may have
     * specified that cached data may not be used in a response.
     *
     * - RFC2616 14.9.2 What May be Stored by Caches. If Cache-Control:
     * no-store arrives, do not serve from or store to the cache.
     */

    /* This value comes from the client's initial request. */
    if (!cache->control_in.parsed) {
        const char *cc_req = cache_table_getm(r->pool, r->headers_in,
                "Cache-Control");
        const char *pragma = cache_table_getm(r->pool, r->headers_in, "Pragma");
        ap_cache_control(r, &cache->control_in, cc_req, pragma, r->headers_in);
    }

    if (cache->control_in.no_store) {

        if (!conf->ignorecachecontrol) {
            /* We're not allowed to serve a cached copy */
            return 0;
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, APLOGNO(02658)
                    "Incoming request is asking for a no-store version of "
                    "%s, but we have been configured to ignore it and serve "
                    "cached content anyway", r->unparsed_uri);
        }
    }

    return 1;
}

int cache_check_freshness(cache_handle_t *h, cache_request_rec *cache,
        request_rec *r)
{
    apr_status_t status;
    apr_int64_t age, maxage_req, maxage_cresp, maxage, smaxage, maxstale;
    apr_int64_t minfresh;
    const char *cc_req;
    const char *pragma;
    const char *agestr = NULL;
    apr_time_t age_c = 0;
    cache_info *info = &(h->cache_obj->info);
    const char *warn_head;
    cache_server_conf *conf =
      (cache_server_conf *)ap_get_module_config(r->server->module_config,
                                                &cache_module);

    /*
     * We now want to check if our cached data is still fresh. This depends
     * on a few things, in this order:
     *
     * - RFC2616 14.9.4 End to end reload, Cache-Control: no-cache. no-cache
     * in either the request or the cached response means that we must
     * perform the request unconditionally, and ignore cached content. We
     * should never reach here, but if we do, mark the content as stale,
     * as this is the best we can do.
     *
     * - RFC2616 14.32 Pragma: no-cache This is treated the same as
     * Cache-Control: no-cache.
     *
     * - RFC2616 14.9.3 Cache-Control: max-stale, must-revalidate,
     * proxy-revalidate if the max-stale request header exists, modify the
     * stale calculations below so that an object can be at most <max-stale>
     * seconds stale before we request a revalidation, _UNLESS_ a
     * must-revalidate or proxy-revalidate cached response header exists to
     * stop us doing this.
     *
     * - RFC2616 14.9.3 Cache-Control: s-maxage the origin server specifies the
     * maximum age an object can be before it is considered stale. This
     * directive has the effect of proxy|must revalidate, which in turn means
     * simple ignore any max-stale setting.
     *
     * - RFC2616 14.9.4 Cache-Control: max-age this header can appear in both
     * requests and responses. If both are specified, the smaller of the two
     * takes priority.
     *
     * - RFC2616 14.21 Expires: if this request header exists in the cached
     * entity, and it's value is in the past, it has expired.
     *
     */

    /* This value comes from the client's initial request. */
    cc_req = apr_table_get(r->headers_in, "Cache-Control");
    pragma = apr_table_get(r->headers_in, "Pragma");

    ap_cache_control(r, &cache->control_in, cc_req, pragma, r->headers_in);

    if (cache->control_in.no_cache) {

        if (!conf->ignorecachecontrol) {
            /* Treat as stale, causing revalidation */
            return 0;
        }

        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, APLOGNO(00781)
                "Incoming request is asking for a uncached version of "
                "%s, but we have been configured to ignore it and "
                "serve a cached response anyway",
                r->unparsed_uri);
    }

    /* These come from the cached entity. */
    if (h->cache_obj->info.control.no_cache
            || h->cache_obj->info.control.invalidated) {
        /*
         * The cached entity contained Cache-Control: no-cache, or a
         * no-cache with a header present, or a private with a header
         * present, or the cached entity has been invalidated in the
         * past, so treat as stale causing revalidation.
         */
        return 0;
    }

    if ((agestr = apr_table_get(h->resp_hdrs, "Age"))) {
        char *endp;
        apr_off_t offt;
        if (!apr_strtoff(&offt, agestr, &endp, 10)
                && endp > agestr && !*endp) {
            age_c = offt;
        }
    }

    /* calculate age of object */
    age = ap_cache_current_age(info, age_c, r->request_time);

    /* extract s-maxage */
    smaxage = h->cache_obj->info.control.s_maxage_value;

    /* extract max-age from request */
    maxage_req = -1;
    if (!conf->ignorecachecontrol) {
        maxage_req = cache->control_in.max_age_value;
    }

    /*
     * extract max-age from response, if both s-maxage and max-age, s-maxage
     * takes priority
     */
    if (smaxage != -1) {
        maxage_cresp = smaxage;
    }
    else {
        maxage_cresp = h->cache_obj->info.control.max_age_value;
    }

    /*
     * if both maxage request and response, the smaller one takes priority
     */
    if (maxage_req == -1) {
        maxage = maxage_cresp;
    }
    else if (maxage_cresp == -1) {
        maxage = maxage_req;
    }
    else {
        maxage = MIN(maxage_req, maxage_cresp);
    }

    /* extract max-stale */
    if (cache->control_in.max_stale) {
        if (cache->control_in.max_stale_value != -1) {
            maxstale = cache->control_in.max_stale_value;
        }
        else {
            /*
             * If no value is assigned to max-stale, then the client is willing
             * to accept a stale response of any age (RFC2616 14.9.3). We will
             * set it to one year in this case as this situation is somewhat
             * similar to a "never expires" Expires header (RFC2616 14.21)
             * which is set to a date one year from the time the response is
             * sent in this case.
             */
            maxstale = APR_INT64_C(86400*365);
        }
    }
    else {
        maxstale = 0;
    }

    /* extract min-fresh */
    if (!conf->ignorecachecontrol && cache->control_in.min_fresh) {
        minfresh = cache->control_in.min_fresh_value;
    }
    else {
        minfresh = 0;
    }

    /* override maxstale if must-revalidate, proxy-revalidate or s-maxage */
    if (maxstale && (h->cache_obj->info.control.must_revalidate
            || h->cache_obj->info.control.proxy_revalidate || smaxage != -1)) {
        maxstale = 0;
    }

    /* handle expiration */
    if (((maxage != -1) && (age < (maxage + maxstale - minfresh))) ||
        ((smaxage == -1) && (maxage == -1) &&
         (info->expire != APR_DATE_BAD) &&
         (age < (apr_time_sec(info->expire - info->date) + maxstale - minfresh)))) {

        warn_head = apr_table_get(h->resp_hdrs, "Warning");

        /* it's fresh darlings... */
        /* set age header on response */
        apr_table_set(h->resp_hdrs, "Age",
                      apr_psprintf(r->pool, "%lu", (unsigned long)age));

        /* add warning if maxstale overrode freshness calculation */
        if (!(((maxage != -1) && age < maxage) ||
              (info->expire != APR_DATE_BAD &&
               (apr_time_sec(info->expire - info->date)) > age))) {
            /* make sure we don't stomp on a previous warning */
            if ((warn_head == NULL) ||
                    (ap_strstr_c(warn_head, "110") == NULL)) {
                apr_table_mergen(h->resp_hdrs, "Warning",
                                 "110 Response is stale");
            }
        }

        /*
         * If none of Expires, Cache-Control: max-age, or Cache-Control:
         * s-maxage appears in the response, and the response header age
         * calculated is more than 24 hours add the warning 113
         */
        if ((maxage_cresp == -1) && (smaxage == -1) && (apr_table_get(
                h->resp_hdrs, "Expires") == NULL) && (age > 86400)) {

            /* Make sure we don't stomp on a previous warning, and don't dup
             * a 113 marning that is already present. Also, make sure to add
             * the new warning to the correct *headers_out location.
             */
            if ((warn_head == NULL) ||
                    (ap_strstr_c(warn_head, "113") == NULL)) {
                apr_table_mergen(h->resp_hdrs, "Warning",
                                 "113 Heuristic expiration");
            }
        }
        return 1;    /* Cache object is fresh (enough) */
    }

    /*
     * At this point we are stale, but: if we are under load, we may let
     * a significant number of stale requests through before the first
     * stale request successfully revalidates itself, causing a sudden
     * unexpected thundering herd which in turn brings angst and drama.
     *
     * So.
     *
     * We want the first stale request to go through as normal. But the
     * second and subsequent request, we must pretend to be fresh until
     * the first request comes back with either new content or confirmation
     * that the stale content is still fresh.
     *
     * To achieve this, we create a very simple file based lock based on
     * the key of the cached object. We attempt to open the lock file with
     * exclusive write access. If we succeed, woohoo! we're first, and we
     * follow the stale path to the backend server. If we fail, oh well,
     * we follow the fresh path, and avoid being a thundering herd.
     *
     * The lock lives only as long as the stale request that went on ahead.
     * If the request succeeds, the lock is deleted. If the request fails,
     * the lock is deleted, and another request gets to make a new lock
     * and try again.
     *
     * At any time, a request marked "no-cache" will force a refresh,
     * ignoring the lock, ensuring an extended lockout is impossible.
     *
     * A lock that exceeds a maximum age will be deleted, and another
     * request gets to make a new lock and try again.
     */
    status = cache_try_lock(conf, cache, r);
    if (APR_SUCCESS == status) {
        /* we obtained a lock, follow the stale path */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(00782)
                "Cache lock obtained for stale cached URL, "
                "revalidating entry: %s",
                r->unparsed_uri);
        return 0;
    }
    else if (APR_STATUS_IS_EEXIST(status)) {
        /* lock already exists, return stale data anyway, with a warning */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, status, r, APLOGNO(00783)
                "Cache already locked for stale cached URL, "
                "pretend it is fresh: %s",
                r->unparsed_uri);

        /* make sure we don't stomp on a previous warning */
        warn_head = apr_table_get(h->resp_hdrs, "Warning");
        if ((warn_head == NULL) ||
                (ap_strstr_c(warn_head, "110") == NULL)) {
            apr_table_mergen(h->resp_hdrs, "Warning",
                             "110 Response is stale");
        }

        return 1;
    }
    else {
        /* some other error occurred, just treat the object as stale */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, status, r, APLOGNO(00784)
                "Attempt to obtain a cache lock for stale "
                "cached URL failed, revalidating entry anyway: %s",
                r->unparsed_uri);
        return 0;
    }

}

/* return each comma separated token, one at a time */
CACHE_DECLARE(const char *)ap_cache_tokstr(apr_pool_t *p, const char *list,
                                           const char **str)
{
    apr_size_t i;
    const char *s;

    s = ap_strchr_c(list, ',');
    if (s != NULL) {
        i = s - list;
        do
            s++;
        while (apr_isspace(*s))
            ; /* noop */
    }
    else
        i = strlen(list);

    while (i > 0 && apr_isspace(list[i - 1]))
        i--;

    *str = s;
    if (i)
        return apr_pstrmemdup(p, list, i);
    else
        return NULL;
}

/*
 * Converts apr_time_t expressed as hex digits to
 * a true apr_time_t.
 */
CACHE_DECLARE(apr_time_t) ap_cache_hex2usec(const char *x)
{
    int i, ch;
    apr_time_t j;
    for (i = 0, j = 0; i < sizeof(j) * 2; i++) {
        ch = x[i];
        j <<= 4;
        if (apr_isdigit(ch))
            j |= ch - '0';
        else if (apr_isupper(ch))
            j |= ch - ('A' - 10);
        else
            j |= ch - ('a' - 10);
    }
    return j;
}

/*
 * Converts apr_time_t to apr_time_t expressed as hex digits.
 */
CACHE_DECLARE(void) ap_cache_usec2hex(apr_time_t j, char *y)
{
    int i, ch;

    for (i = (sizeof(j) * 2)-1; i >= 0; i--) {
        ch = (int)(j & 0xF);
        j >>= 4;
        if (ch >= 10)
            y[i] = ch + ('A' - 10);
        else
            y[i] = ch + '0';
    }
    y[sizeof(j) * 2] = '\0';
}

static void cache_hash(const char *it, char *val, int ndepth, int nlength)
{
    apr_md5_ctx_t context;
    unsigned char digest[16];
    char tmp[22];
    int i, k, d;
    unsigned int x;
    static const char enc_table[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_@";

    apr_md5_init(&context);
    apr_md5_update(&context, (const unsigned char *) it, strlen(it));
    apr_md5_final(digest, &context);

    /* encode 128 bits as 22 characters, using a modified uuencoding
     * the encoding is 3 bytes -> 4 characters* i.e. 128 bits is
     * 5 x 3 bytes + 1 byte -> 5 * 4 characters + 2 characters
     */
    for (i = 0, k = 0; i < 15; i += 3) {
        x = (digest[i] << 16) | (digest[i + 1] << 8) | digest[i + 2];
        tmp[k++] = enc_table[x >> 18];
        tmp[k++] = enc_table[(x >> 12) & 0x3f];
        tmp[k++] = enc_table[(x >> 6) & 0x3f];
        tmp[k++] = enc_table[x & 0x3f];
    }

    /* one byte left */
    x = digest[15];
    tmp[k++] = enc_table[x >> 2];    /* use up 6 bits */
    tmp[k++] = enc_table[(x << 4) & 0x3f];

    /* now split into directory levels */
    for (i = k = d = 0; d < ndepth; ++d) {
        memcpy(&val[i], &tmp[k], nlength);
        k += nlength;
        val[i + nlength] = '/';
        i += nlength + 1;
    }
    memcpy(&val[i], &tmp[k], 22 - k);
    val[i + 22 - k] = '\0';
}

CACHE_DECLARE(char *)ap_cache_generate_name(apr_pool_t *p, int dirlevels,
                                            int dirlength, const char *name)
{
    char hashfile[66];
    cache_hash(name, hashfile, dirlevels, dirlength);
    return apr_pstrdup(p, hashfile);
}

/**
 * String tokenizer per RFC 7234 section 5.2 (1#token[=["]arg["]]).
 * If any (and arg not NULL), the argument is also returned (unquoted).
 */
apr_status_t cache_strqtok(char *str, char **token, char **arg, char **last)
{
#define CACHE_TOKEN_SEPS "\t ,"
    apr_status_t rv = APR_SUCCESS;
    enum {
        IN_TOKEN,
        IN_BETWEEN,
        IN_ARGUMENT,
        IN_QUOTES
    } state = IN_TOKEN;
    char *wpos;

    if (!str) {         /* subsequent call */
        str = *last;    /* start where we left off */
    }
    if (!str) {         /* no more tokens */
        return APR_EOF;
    }

    /* skip separators (will terminate at '\0') */
    while (*str && TEST_CHAR(*str, T_HTTP_TOKEN_STOP)) {
        if (!ap_strchr_c(CACHE_TOKEN_SEPS, *str)) {
            return APR_EINVAL;
        }
        ++str;
    }
    if (!*str) {        /* no more tokens */
        return APR_EOF;
    }

    *token = str;
    if (arg) {
        *arg = NULL;
    }

    /* skip valid token characters to terminate token and
     * prepare for the next call (will terminate at '\0)
     * on the way, handle quoted strings, and within
     * quoted strings, escaped characters.
     */
    for (wpos = str; *str; ++str) {
        switch (state) {
        case IN_TOKEN:
            if (*str == '=') {
                state = IN_BETWEEN;
                *wpos++ = '\0';
                if (arg) *arg = wpos;
                continue;
            }
            if (TEST_CHAR(*str, T_HTTP_TOKEN_STOP)) {
                goto end;
            }
            break;

        case IN_BETWEEN:
            if (*str == '"') {
                state = IN_QUOTES;
                continue;
            }
            /* fall through */
            state = IN_ARGUMENT;
        case IN_ARGUMENT:
            if (TEST_CHAR(*str, T_HTTP_TOKEN_STOP)) {
                goto end;
            }
            break;

        default:
            AP_DEBUG_ASSERT(state == IN_QUOTES);
            if (*str == '"') {
                ++str;
                goto end;
            }
            if (*str == '\\' && *(str + 1)) {
                ++str;
            }
            break;
        }
        *wpos++ = *str;
    }
end:
    /* anything after should be trailing OWS or comma */
    for (; *str; ++str) {
        if (*str == ',') {
            ++str;
            break;
        }
        if (*str != '\t' && *str != ' ') {
            rv = APR_EINVAL;
            break;
        }
    }

    *wpos = '\0';
    *last = str;
    return rv;
}

/**
 * Parse the Cache-Control and Pragma headers in one go, marking
 * which tokens appear within the header. Populate the structure
 * passed in.
 */
int ap_cache_control(request_rec *r, cache_control_t *cc,
        const char *cc_header, const char *pragma_header, apr_table_t *headers)
{
    char *last;
    apr_status_t rv;

    if (cc->parsed) {
        return cc->cache_control || cc->pragma;
    }

    cc->parsed = 1;
    cc->max_age_value = -1;
    cc->max_stale_value = -1;
    cc->min_fresh_value = -1;
    cc->s_maxage_value = -1;

    if (pragma_header) {
        char *header = apr_pstrdup(r->pool, pragma_header), *token;
        for (rv = cache_strqtok(header, &token, NULL, &last);
             rv == APR_SUCCESS;
             rv = cache_strqtok(NULL, &token, NULL, &last)) {
            if (!ap_cstr_casecmp(token, "no-cache")) {
                cc->no_cache = 1;
            }
        }
        cc->pragma = 1;
    }

    if (cc_header) {
        char *header = apr_pstrdup(r->pool, cc_header), *token, *arg;
        for (rv = cache_strqtok(header, &token, &arg, &last);
             rv == APR_SUCCESS;
             rv = cache_strqtok(NULL, &token, &arg, &last)) {
            char *endp;
            apr_off_t offt;

            switch (token[0]) {
            case 'n':
            case 'N':
                if (!ap_cstr_casecmp(token, "no-cache")) {
                    if (!arg) {
                        cc->no_cache = 1;
                    }
                    else {
                        cc->no_cache_header = 1;
                    }
                }
                else if (!ap_cstr_casecmp(token, "no-store")) {
                    cc->no_store = 1;
                }
                else if (!ap_cstr_casecmp(token, "no-transform")) {
                    cc->no_transform = 1;
                }
                break;

            case 'm':
            case 'M':
                if (arg && !ap_cstr_casecmp(token, "max-age")) {
                    if (!apr_strtoff(&offt, arg, &endp, 10)
                            && endp > arg && !*endp) {
                        cc->max_age = 1;
                        cc->max_age_value = offt;
                    }
                }
                else if (!ap_cstr_casecmp(token, "must-revalidate")) {
                    cc->must_revalidate = 1;
                }
                else if (!ap_cstr_casecmp(token, "max-stale")) {
                    if (!arg) {
                        cc->max_stale = 1;
                        cc->max_stale_value = -1;
                    }
                    else if (!apr_strtoff(&offt, arg, &endp, 10)
                             && endp > arg && !*endp) {
                        cc->max_stale = 1;
                        cc->max_stale_value = offt;
                    }
                }
                else if (arg && !ap_cstr_casecmp(token, "min-fresh")) {
                    if (!apr_strtoff(&offt, arg, &endp, 10)
                            && endp > arg && !*endp) {
                        cc->min_fresh = 1;
                        cc->min_fresh_value = offt;
                    }
                }
                break;

            case 'o':
            case 'O':
                if (!ap_cstr_casecmp(token, "only-if-cached")) {
                    cc->only_if_cached = 1;
                }
                break;

            case 'p':
            case 'P':
                if (!ap_cstr_casecmp(token, "public")) {
                    cc->public = 1;
                }
                else if (!ap_cstr_casecmp(token, "private")) {
                    if (!arg) {
                        cc->private = 1;
                    }
                    else {
                        cc->private_header = 1;
                    }
                }
                else if (!ap_cstr_casecmp(token, "proxy-revalidate")) {
                    cc->proxy_revalidate = 1;
                }
                break;

            case 's':
            case 'S':
                if (arg && !ap_cstr_casecmp(token, "s-maxage")) {
                    if (!apr_strtoff(&offt, arg, &endp, 10)
                            && endp > arg && !*endp) {
                        cc->s_maxage = 1;
                        cc->s_maxage_value = offt;
                    }
                }
                break;
            }
        }
        cc->cache_control = 1;
    }

    return (cc_header != NULL || pragma_header != NULL);
}

/**
 * Parse the Cache-Control, identifying and removing headers that
 * exist as tokens after the no-cache and private tokens.
 */
static int cache_control_remove(request_rec *r, const char *cc_header,
        apr_table_t *headers)
{
    char *last, *slast, *sheader;
    int found = 0;

    if (cc_header) {
        apr_status_t rv;
        char *header = apr_pstrdup(r->pool, cc_header), *token, *arg;
        for (rv = cache_strqtok(header, &token, &arg, &last);
             rv == APR_SUCCESS;
             rv = cache_strqtok(NULL, &token, &arg, &last)) {
            if (!arg) {
                continue;
            }

            switch (token[0]) {
            case 'n':
            case 'N':
                if (!ap_cstr_casecmp(token, "no-cache")) {
                    for (rv = cache_strqtok(arg, &sheader, NULL, &slast);
                         rv == APR_SUCCESS;
                         rv = cache_strqtok(NULL, &sheader, NULL, &slast)) {
                        apr_table_unset(headers, sheader);
                    }
                    found = 1;
                }
                break;

            case 'p':
            case 'P':
                if (!ap_cstr_casecmp(token, "private")) {
                    for (rv = cache_strqtok(arg, &sheader, NULL, &slast);
                         rv == APR_SUCCESS;
                         rv = cache_strqtok(NULL, &sheader, NULL, &slast)) {
                        apr_table_unset(headers, sheader);
                    }
                    found = 1;
                }
                break;
            }
        }
    }

    return found;
}

/*
 * Create a new table consisting of those elements from an
 * headers table that are allowed to be stored in a cache.
 */
CACHE_DECLARE(apr_table_t *)ap_cache_cacheable_headers(apr_pool_t *pool,
                                                        apr_table_t *t,
                                                        server_rec *s)
{
    cache_server_conf *conf;
    char **header;
    int i;
    apr_table_t *headers_out;

    /* Short circuit the common case that there are not
     * (yet) any headers populated.
     */
    if (t == NULL) {
        return apr_table_make(pool, 10);
    };

    /* Make a copy of the headers, and remove from
     * the copy any hop-by-hop headers, as defined in Section
     * 13.5.1 of RFC 2616
     */
    headers_out = apr_table_copy(pool, t);

    apr_table_unset(headers_out, "Connection");
    apr_table_unset(headers_out, "Keep-Alive");
    apr_table_unset(headers_out, "Proxy-Authenticate");
    apr_table_unset(headers_out, "Proxy-Authorization");
    apr_table_unset(headers_out, "TE");
    apr_table_unset(headers_out, "Trailers");
    apr_table_unset(headers_out, "Transfer-Encoding");
    apr_table_unset(headers_out, "Upgrade");

    conf = (cache_server_conf *)ap_get_module_config(s->module_config,
                                                     &cache_module);

    /* Remove the user defined headers set with CacheIgnoreHeaders.
     * This may break RFC 2616 compliance on behalf of the administrator.
     */
    header = (char **)conf->ignore_headers->elts;
    for (i = 0; i < conf->ignore_headers->nelts; i++) {
        apr_table_unset(headers_out, header[i]);
    }
    return headers_out;
}

/*
 * Create a new table consisting of those elements from an input
 * headers table that are allowed to be stored in a cache.
 */
CACHE_DECLARE(apr_table_t *)ap_cache_cacheable_headers_in(request_rec *r)
{
    return ap_cache_cacheable_headers(r->pool, r->headers_in, r->server);
}

/*
 * Create a new table consisting of those elements from an output
 * headers table that are allowed to be stored in a cache;
 * ensure there is a content type and capture any errors.
 */
CACHE_DECLARE(apr_table_t *)ap_cache_cacheable_headers_out(request_rec *r)
{
    apr_table_t *headers_out;

    headers_out = ap_cache_cacheable_headers(r->pool,
                                             cache_merge_headers_out(r),
                                             r->server);

    cache_control_remove(r,
            cache_table_getm(r->pool, headers_out, "Cache-Control"),
            headers_out);

    return headers_out;
}

apr_table_t *cache_merge_headers_out(request_rec *r)
{
    apr_table_t *headers_out;

    headers_out = apr_table_overlay(r->pool, r->headers_out,
                                    r->err_headers_out);

    if (r->content_type
            && !apr_table_get(headers_out, "Content-Type")) {
        const char *ctype = ap_make_content_type(r, r->content_type);
        if (ctype) {
            apr_table_setn(headers_out, "Content-Type", ctype);
        }
    }

    if (r->content_encoding
            && !apr_table_get(headers_out, "Content-Encoding")) {
        apr_table_setn(headers_out, "Content-Encoding",
                       r->content_encoding);
    }

    return headers_out;
}

typedef struct
{
    apr_pool_t *p;
    const char *first;
    apr_array_header_t *merged;
} cache_table_getm_t;

static int cache_table_getm_do(void *v, const char *key, const char *val)
{
    cache_table_getm_t *state = (cache_table_getm_t *) v;

    if (!state->first) {
        /**
         * The most common case is a single header, and this is covered by
         * a fast path that doesn't allocate any memory. On the second and
         * subsequent header, an array is created and the array concatenated
         * together to form the final value.
         */
        state->first = val;
    }
    else {
        const char **elt;
        if (!state->merged) {
            state->merged = apr_array_make(state->p, 10, sizeof(const char *));
            elt = apr_array_push(state->merged);
            *elt = state->first;
        }
        elt = apr_array_push(state->merged);
        *elt = val;
    }
    return 1;
}

const char *cache_table_getm(apr_pool_t *p, const apr_table_t *t,
        const char *key)
{
    cache_table_getm_t state;

    state.p = p;
    state.first = NULL;
    state.merged = NULL;

    apr_table_do(cache_table_getm_do, &state, t, key, NULL);

    if (!state.first) {
        return NULL;
    }
    else if (!state.merged) {
        return state.first;
    }
    else {
        return apr_array_pstrcat(p, state.merged, ',');
    }
}

/*
 * QUERY (RFC 10008) support: the cache key incorporates the request
 * content and its metadata (Section 2.7).  The content is hashed with
 * SHA-256, which apr-util 1.x does not provide, hence the small
 * implementation below (FIPS 180-4).
 */

typedef struct {
    apr_uint32_t h[8];
    apr_uint64_t len;           /* total bytes */
    unsigned char buf[64];
    apr_size_t buflen;
} cache_sha256_ctx;

static const apr_uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) ((((x) >> (n)) | ((x) << (32 - (n)))) & 0xffffffff)

static void sha256_init(cache_sha256_ctx *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->buflen = 0;
}

static void sha256_block(cache_sha256_ctx *c, const unsigned char *p)
{
    apr_uint32_t w[64], v[8], s0, s1, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((apr_uint32_t)p[4 * i] << 24) | ((apr_uint32_t)p[4 * i + 1] << 16)
               | ((apr_uint32_t)p[4 * i + 2] << 8) | (apr_uint32_t)p[4 * i + 3];
    }
    for (; i < 64; i++) {
        s0 = SHA256_ROTR(w[i - 15], 7) ^ SHA256_ROTR(w[i - 15], 18)
             ^ (w[i - 15] >> 3);
        s1 = SHA256_ROTR(w[i - 2], 17) ^ SHA256_ROTR(w[i - 2], 19)
             ^ (w[i - 2] >> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & 0xffffffff;
    }
    for (i = 0; i < 8; i++) {
        v[i] = c->h[i];
    }
    for (i = 0; i < 64; i++) {
        s1 = SHA256_ROTR(v[4], 6) ^ SHA256_ROTR(v[4], 11)
             ^ SHA256_ROTR(v[4], 25);
        t1 = (v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha256_k[i]
              + w[i]) & 0xffffffff;
        s0 = SHA256_ROTR(v[0], 2) ^ SHA256_ROTR(v[0], 13)
             ^ SHA256_ROTR(v[0], 22);
        t2 = (s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2])))
             & 0xffffffff;
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = (v[3] + t1) & 0xffffffff;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = (t1 + t2) & 0xffffffff;
    }
    for (i = 0; i < 8; i++) {
        c->h[i] = (c->h[i] + v[i]) & 0xffffffff;
    }
}

static void sha256_update(cache_sha256_ctx *c, const void *data,
                          apr_size_t len)
{
    const unsigned char *p = data;

    c->len += len;
    while (len > 0) {
        apr_size_t n = 64 - c->buflen;

        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
        p += n;
        len -= n;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(cache_sha256_ctx *c, unsigned char digest[32])
{
    apr_uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    unsigned char lenbuf[8];
    int i;

    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->buflen != 56) {
        sha256_update(c, &pad, 1);
    }
    for (i = 0; i < 8; i++) {
        lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha256_update(c, lenbuf, 8);
    for (i = 0; i < 8; i++) {
        digest[4 * i] = (unsigned char)(c->h[i] >> 24);
        digest[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        digest[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        digest[4 * i + 3] = (unsigned char)c->h[i];
    }
}

/* Lower-case the media type and coding names, drop optional whitespace;
 * parameter values are kept as they are.
 */
static const char *query_key_field(apr_pool_t *p, const char *value)
{
    char *v, *s, *d;
    int in_type = 1, quoted = 0;

    if (!value) {
        return "";
    }
    v = apr_pstrdup(p, value);
    for (s = d = v; *s; ++s) {
        if (*s == '"') {
            quoted = !quoted;
        }
        else if (!quoted && (*s == ' ' || *s == '\t')) {
            continue;
        }
        else if (!quoted && *s == ';') {
            in_type = 0;
        }
        else if (!quoted && *s == ',') {
            in_type = 1;
        }
        *d++ = in_type ? apr_tolower(*s) : *s;
    }
    *d = '\0';
    return v;
}

#define CACHE_QUERY_REPLAY_KEY "mod_cache-query-replay"

typedef struct {
    apr_bucket_brigade *bb;     /* buffered content still to replay */
    apr_status_t rv;            /* read error to report after the content */
    apr_off_t length;           /* content read so far, buffered or not */
    apr_off_t limit;            /* LimitRequestBody of the handler's config */
    unsigned int started:1;     /* the handler has begun to read */
    unsigned int too_large:1;   /* the content exceeds the limit */
    char hex[65];               /* SHA-256 for the key, "" if not cacheable */
} cache_query_replay_ctx;

void cache_query_replay_insert(request_rec *r, ap_filter_rec_t *replay)
{
    cache_query_replay_ctx *ctx;
    ap_filter_t *f;
    void *data = NULL;

    /* Only an internal redirect loses the filter; a subrequest has its
     * own pool and inherits the input filters of its parent.
     */
    if (!r->prev) {
        return;
    }
    apr_pool_userdata_get(&data, CACHE_QUERY_REPLAY_KEY, r->pool);
    ctx = data;
    if (!ctx || (ctx->started && !ctx->limit && ctx->rv == APR_SUCCESS
                 && APR_BRIGADE_EMPTY(ctx->bb))) {
        return;
    }
    for (f = r->input_filters; f; f = f->next) {
        if (f->frec == replay) {
            return;
        }
    }
    /* the limit is that of the configuration of the new request */
    ctx->started = 0;
    ap_add_input_filter_handle(replay, ctx, r, r->connection);
}

int cache_query_key(cache_request_rec *cache, request_rec *r,
                    cache_server_conf *conf, ap_filter_rec_t *replay)
{
    cache_query_replay_ctx *ctx;
    cache_sha256_ctx sha;
    apr_bucket_brigade *tmp;
    const char *ctype, *cenc, *clen, *key = NULL;
    unsigned char digest[32];
    apr_off_t total = 0;
    int seen_eos = 0;
    apr_status_t rv;
    void *data = NULL;
    int i;

    /* An internal redirect runs the cache again on the same request
     * content: it was read and hashed the first time.
     */
    apr_pool_userdata_get(&data, CACHE_QUERY_REPLAY_KEY, r->pool);
    if (data) {
        ctx = data;
        cache_query_replay_insert(r, replay);
        if (!ctx->hex[0]) {
            return DECLINED;
        }
        goto make_key;
    }

    ctype = apr_table_get(r->headers_in, "Content-Type");
    if (!ctype) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(10682)
                      "cache: QUERY without Content-Type not cached: %s",
                      r->uri);
        return DECLINED;
    }
    clen = apr_table_get(r->headers_in, "Content-Length");
    if (clen) {
        apr_off_t len;

        if (!ap_parse_strict_length(&len, clen)
            || len > conf->query_maxbody) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(10683)
                          "cache: QUERY content length %s exceeds "
                          "CacheQueryMaxBodySize, not cached: %s",
                          clen, r->uri);
            return DECLINED;
        }
    }

    sha256_init(&sha);
    ctype = query_key_field(r->pool, ctype);
    cenc = query_key_field(r->pool,
                           apr_table_get(r->headers_in, "Content-Encoding"));
    sha256_update(&sha, ctype, strlen(ctype));
    sha256_update(&sha, "\n", 1);
    sha256_update(&sha, cenc, strlen(cenc));
    sha256_update(&sha, "\n", 1);

    /* Buffer the content (up to the limit), and replay it to the handler
     * in any case, so that a request which cannot be cached still reaches
     * the handler unchanged.
     */
    ctx = apr_pcalloc(r->pool, sizeof(*ctx));
    ctx->bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    tmp = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    do {
        rv = ap_get_brigade(r->input_filters, tmp, AP_MODE_READBYTES,
                            APR_BLOCK_READ, HUGE_STRING_LEN);
        if (rv != APR_SUCCESS) {
            ctx->rv = rv;
            break;
        }
        while (!APR_BRIGADE_EMPTY(tmp)) {
            apr_bucket *e = APR_BRIGADE_FIRST(tmp);

            if (APR_BUCKET_IS_EOS(e)) {
                seen_eos = 1;
            }
            else if (!APR_BUCKET_IS_METADATA(e)) {
                const char *data;
                apr_size_t len;

                rv = apr_bucket_read(e, &data, &len, APR_BLOCK_READ);
                if (rv != APR_SUCCESS) {
                    ctx->rv = rv;
                    break;
                }
                sha256_update(&sha, data, len);
                total += len;
            }
            rv = apr_bucket_setaside(e, r->pool);
            if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
                ctx->rv = rv;
                break;
            }
            APR_BUCKET_REMOVE(e);
            APR_BRIGADE_INSERT_TAIL(ctx->bb, e);
        }
        apr_brigade_cleanup(tmp);
    } while (!seen_eos && ctx->rv == APR_SUCCESS
             && total <= conf->query_maxbody);

    /* The request pool outlives internal redirects, which drop the
     * filter: cache_query_replay_insert() finds the content here.
     */
    ctx->length = total;
    apr_pool_userdata_setn(ctx, CACHE_QUERY_REPLAY_KEY, NULL, r->pool);
    ap_add_input_filter_handle(replay, ctx, r, r->connection);

    if (ctx->rv != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, ctx->rv, r, APLOGNO(10684)
                      "cache: reading QUERY content failed, not cached: %s",
                      r->uri);
        return DECLINED;
    }
    if (!seen_eos) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(10685)
                      "cache: QUERY content exceeds CacheQueryMaxBodySize "
                      "%" APR_OFF_T_FMT ", not cached: %s",
                      conf->query_maxbody, r->uri);
        return DECLINED;
    }

    sha256_final(&sha, digest);
    for (i = 0; i < (int)sizeof(digest); i++) {
        ctx->hex[2 * i] = "0123456789abcdef"[digest[i] >> 4];
        ctx->hex[2 * i + 1] = "0123456789abcdef"[digest[i] & 0x0f];
    }
    ctx->hex[2 * sizeof(digest)] = '\0';

make_key:
    if (cache_generate_key(r, r->pool, &key) != APR_SUCCESS) {
        return DECLINED;
    }

    /* A fragment never occurs in a request target, so this can not
     * collide with the key of any GET request.
     */
    cache->key = apr_pstrcat(r->pool, key, "#query=", ctx->hex, NULL);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(10686)
                  "cache: Key for QUERY %s is %s", r->uri, cache->key);
    return OK;
}

/* The content was read before the per-directory configuration of the
 * request was known, with the LimitRequestBody of the server in effect.
 * Apply the limit of the handler's configuration, like HTTP_IN does.
 */
static int query_replay_too_large(ap_filter_t *f, cache_query_replay_ctx *ctx,
                                  apr_off_t length)
{
    if (ctx->limit && length > ctx->limit) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, f->r, APLOGNO(10689)
                      "cache: QUERY content length of %" APR_OFF_T_FMT
                      " is larger than the configured limit of %"
                      APR_OFF_T_FMT, length, ctx->limit);
        ctx->too_large = 1;
    }
    return ctx->too_large;
}

apr_status_t cache_query_replay_filter(ap_filter_t *f, apr_bucket_brigade *bb,
                                       ap_input_mode_t mode,
                                       apr_read_type_e block,
                                       apr_off_t readbytes)
{
    cache_query_replay_ctx *ctx = f->ctx;
    apr_bucket *e, *copy;
    apr_status_t rv;

    if (!ctx) {
        ap_remove_input_filter(f);
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }
    if (!ctx->started) {
        const char *clen = apr_table_get(f->r->headers_in, "Content-Length");
        apr_off_t length = ctx->length;

        ctx->started = 1;
        ctx->limit = ap_get_limit_req_body(f->r);
        if (clen && (!ap_parse_strict_length(&length, clen)
                     || length < ctx->length)) {
            length = ctx->length;
        }
        query_replay_too_large(f, ctx, length);
    }
    if (ctx->too_large) {
        return APR_ENOSPC;
    }
    if (APR_BRIGADE_EMPTY(ctx->bb) && ctx->rv == APR_SUCCESS) {
        apr_off_t length = 0;

        if (!ctx->limit) {
            ap_remove_input_filter(f);
            return ap_get_brigade(f->next, bb, mode, block, readbytes);
        }
        /* content beyond CacheQueryMaxBodySize, not buffered */
        rv = ap_get_brigade(f->next, bb, mode, block, readbytes);
        if (rv == APR_SUCCESS && mode != AP_MODE_SPECULATIVE
            && apr_brigade_length(bb, 0, &length) == APR_SUCCESS
            && length > 0) {
            ctx->length += length;
            if (query_replay_too_large(f, ctx, ctx->length)) {
                apr_brigade_cleanup(bb);
                return APR_ENOSPC;
            }
        }
        return rv;
    }
    if (APR_BRIGADE_EMPTY(ctx->bb)) {
        /* report the read error once, as it happened */
        rv = ctx->rv;
        ctx->rv = APR_SUCCESS;
        return rv;
    }

    switch (mode) {
    case AP_MODE_EXHAUSTIVE:
        APR_BRIGADE_CONCAT(bb, ctx->bb);
        return APR_SUCCESS;

    case AP_MODE_GETLINE:
        return apr_brigade_split_line(bb, ctx->bb, block, HUGE_STRING_LEN);

    case AP_MODE_READBYTES:
    case AP_MODE_SPECULATIVE:
        rv = apr_brigade_partition(ctx->bb, readbytes, &e);
        if (rv != APR_SUCCESS && rv != APR_INCOMPLETE) {
            return rv;
        }
        if (mode == AP_MODE_SPECULATIVE) {
            apr_bucket *b;

            for (b = APR_BRIGADE_FIRST(ctx->bb); b != e;
                 b = APR_BUCKET_NEXT(b)) {
                rv = apr_bucket_copy(b, &copy);
                if (rv != APR_SUCCESS) {
                    return rv;
                }
                APR_BRIGADE_INSERT_TAIL(bb, copy);
            }
        }
        else {
            while (APR_BRIGADE_FIRST(ctx->bb) != e) {
                apr_bucket *b = APR_BRIGADE_FIRST(ctx->bb);

                APR_BUCKET_REMOVE(b);
                APR_BRIGADE_INSERT_TAIL(bb, b);
            }
        }
        return APR_SUCCESS;

    default:
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }
}
