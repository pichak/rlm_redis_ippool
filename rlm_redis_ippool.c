/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 if the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <freeradius-devel/ident.h>

/**
 * $Id: 3b250c4f890164d0e35f54e9d9319f280942a0df $
 * @file rlm_redis_ippool.c
 * @brief Redis ippool module code.
 *
 * @copyright 2013 The FreeRADIUS server project
 * @copyright 2013 your name \<your address\>
 */
RCSID("$Id: 3b250c4f890164d0e35f54e9d9319f280942a0df $")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>

#include <ctype.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <rlm_redis.h>

/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct rlm_redis_ippool_t {
	const char *xlat_name;

	char *redis_instance_name;
	REDIS_INST *redis_inst;

	int ip_key;

	int max_collision_retry;

	/*
	 * 	expire time in seconds if no updates are received for a user
	 */
	int expire_time;

	char *get_pool_range;

	char *allocate_check;
	char *allocate;

	char *allocate_update;

	char *deallocate;
} rlm_redis_ippool_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{ "redis-instance-name", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, redis_instance_name), NULL, "redis"},

	{ "ip-key", PW_TYPE_INTEGER,
	  offsetof(rlm_redis_ippool_t, ip_key), NULL, ""},

	{ "max-collision-retry", PW_TYPE_INTEGER,
	  offsetof(rlm_redis_ippool_t, max_collision_retry), NULL, "4"},

	{ "expire-time", PW_TYPE_INTEGER,
	  offsetof(rlm_redis_ippool_t, expire_time), NULL, "3600"},

	{ "get-pool-range", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, get_pool_range), NULL, ""},

	{ "allocate-check", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, allocate_check), NULL, ""},
	{ "allocate", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, allocate), NULL, ""},

	{ "allocate-update", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, allocate_update), NULL, ""},

	{ "deallocate", PW_TYPE_STRING_PTR,
	  offsetof(rlm_redis_ippool_t, deallocate), NULL, ""},

	{ NULL, -1, 0, NULL, NULL}
};



/*
 *	Query the database executing a command.
 *	If the result is a positive integer, return that value.
 */
static int redis_ippool_command(const char *fmt, REDISSOCK *dissocket,
			    rlm_redis_ippool_t *data, REQUEST *request)
{
	int result = 0;

	if (data->redis_inst->redis_query(dissocket, data->redis_inst,
	                                  fmt, request) < 0) {

		radlog(L_ERR, "redis_ippool_command: database query error in: '%s'", fmt);
		return -1;
	}

	switch (dissocket->reply->type) {
	case REDIS_REPLY_INTEGER:
		DEBUG("redis_ippool_command: query response %lld\n",
		      dissocket->reply->integer);
		if (dissocket->reply->integer > 0)
		      result = dissocket->reply->integer;
		break;
	case REDIS_REPLY_STATUS:
	case REDIS_REPLY_STRING:
		DEBUG("redis_ippool_command: query response %s\n",
		      dissocket->reply->str);
		break;
	default:
		break;
	}

	return result;
}

static int redis_ippool_detach(UNUSED void *instance);

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int redis_ippool_instantiate(CONF_SECTION *conf, void **instance)
{
	module_instance_t *modinst;
	rlm_redis_ippool_t *inst;

	/*
	 *	Set up a storage area for instance data
	 */
	inst = *instance = rad_malloc(sizeof (*inst));
	memset(inst, 0, sizeof (*inst));
    
	/*
	 *	If the configuration parameters can't be parsed, then
	 *	fail.
	 */
	if (cf_section_parse(conf, inst, module_config) < 0) {
		free(inst);
		return -1;
	}


	modinst = find_module_instance(cf_section_find("modules"),
				       inst->redis_instance_name, 1);
	if (!modinst) {
		radlog(L_ERR,
		       "redis_ippool: failed to find module instance \"%s\"",
		       inst->redis_instance_name);

		redis_ippool_detach(inst);
		return -1;
	}

	if (strcmp(modinst->entry->name, "rlm_redis") != 0) {
		radlog(L_ERR, "redis_ippool: Module \"%s\""
		       " is not an instance of the redis module",
		       inst->redis_instance_name);

		redis_ippool_detach(inst);
		return -1;
	}

	inst->redis_inst = (REDIS_INST *) modinst->insthandle;

	return 0;
}


#define REDIS_COMMAND(_a, _b, _c, _d) rc = redis_ippool_command(_a, _b, _c, _d); if (rc < 0) return RLM_MODULE_FAIL


static char* get_ip_str(long start[4], long ip_key){
	long ip_a, ip_b, ip_c, ip_d;
	
	ip_key += start[3];
	ip_d = ip_key % 255;

	ip_key /= 255;

	ip_key += start[2];
	ip_c = ip_key % 255;

	ip_key /= 255;

	ip_key += start[1];
	ip_b = ip_key % 255;

	ip_key /= 255;

	ip_key += start[0];
	ip_a = ip_key;

	if (ip_c == 0 && ip_d == 0)
		ip_d = 1;

	char result[100];
	sprintf(result, "%d.%d.%d.%d", ip_a, ip_b, ip_c, ip_d);

	return result;
}

static int is_ip_available(char *ip_str, REDISSOCK *dissocket,rlm_redis_ippool_t *data, REQUEST *request){
	int rc;

	VALUE_PAIR *proposed_ip;
	proposed_ip = pairmake("Framed-IP-Address", ip_str, T_OP_SET);
	pairadd(&request->reply->vps, proposed_ip);
	REDIS_COMMAND(data->allocate_check, dissocket, data, request);
	(data->redis_inst->redis_finish_query)(dissocket);

	return strcmp(dissocket->reply->str, '(nil)') == 0;
}


/*
 *	Allocate an IP number from the pool.
 */
static int redis_ippool_post_auth(void * instance, REQUEST * request)
{
	int rc;
	int rcode = RLM_MODULE_NOOP;
	VALUE_PAIR * vp;
	int acct_status_type;
	rlm_redis_ippool_t * data = (rlm_redis_ippool_t *) instance;
	REDISSOCK *dissocket;
	
	char *pool_name = NULL;
	char *pool_ip_range = NULL;

	/*
	 *	If there is a Framed-IP-Address attribute in the reply do nothing
	 */
	if (pairfind(request->reply->vps, PW_FRAMED_IP_ADDRESS) != NULL) {
		RDEBUG("Framed-IP-Address already exists");

		return RLM_MODULE_NOOP;
	}

	if ((vp = pairfind(request->config_items, PW_POOL_NAME)) == NULL) {
		RDEBUG("No Pool-Name defined");

		return RLM_MODULE_NOOP;
	}

	pool_name = vp->vp_strvalue;

	dissocket = data->redis_inst->redis_get_socket(data->redis_inst);
	if (dissocket == NULL) {
		RDEBUG("cannot allocate redis connection");
		return RLM_MODULE_FAIL;
	}

	REDIS_COMMAND(data->get_pool_range, dissocket, data, request);
	pool_ip_range = dissocket->reply->str;

	if (strcmp(pool_ip_range, "(nil)") == 0){
		RDEBUG("Pool with name '%s' not found",pool_name);

		return RLM_MODULE_NOOP;
	}

	// parse ip_range
	int start[4];
	int end[4];
	int iprange_parsed = sscanf(pool_ip_range, "%d.%d.%d.%d %d.%d.%d.%d", start, start+1, start+2, start+3, end, end+1, end+2, end+3);
	if (iprange_parsed != 8){
		if (strcmp(pool_ip_range, "(nil)") == 0){
			RDEBUG("Pool '%s' ip range is invalid",pool_name);

			return RLM_MODULE_NOOP;
		}
	}

	// calc ip range length
	long range_length = (long)(end[0]-start[0])*(255*255*255) + (end[1]-start[1])*(255*255) + (end[2]-start[2])*(255) + end[3]-start[3];

	long ip_key = data->ip_key;

	ip_key %= range_length;

	long ip[4];
	char *ip_str;
	int ip_found = 0;

	int i;
	for (i = 0; i < data->max_collision_retry; ++i)
	{
		ip_str = get_ip_str(start, ip_key);
		if (is_ip_available(ip_str, dissocket, data, request)){
			ip_found = 1;
			break;
		}else{
			ip_key += rand();
			ip_key %= range_length;
		}
	}

	if (!ip_found){
		RDEBUG("Failed to find free ip from pool '%s'",pool_name);
		return RLM_MODULE_FAIL;
	}

	// finish query
	(data->redis_inst->redis_finish_query)(dissocket);

	// allocate ip address
	VALUE_PAIR *proposed_ip;
	proposed_ip = pairmake("Framed-IP-Address", ip_str, T_OP_SET);
	pairadd(&request->reply->vps, proposed_ip);
	REDIS_COMMAND(data->allocate, dissocket, data, request);
	(data->redis_inst->redis_finish_query)(dissocket);

	VALUE_PAIR *framed_ip_address;
	framed_ip_address = pairmake("Framed-IP-Address", ip_str, T_OP_SET);
	pairadd(&request->reply->vps, framed_ip_address);

	return RLM_MODULE_OK;
}




/*
 *	Check for an Accounting-Stop to deallocate ip or Accounting-Update to update ip's time to live
 */
static int redis_ippool_accounting(void * instance, REQUEST * request)
{
	int rc;
	int rcode = RLM_MODULE_NOOP;
	VALUE_PAIR * vp;
	int acct_status_type;
	rlm_redis_ippool_t * data = (rlm_redis_ippool_t *) instance;
	REDISSOCK *dissocket;

	vp = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE);
	if (!vp) {
		RDEBUG("Could not find account status type in packet.");
		return RLM_MODULE_NOOP;
	}
	acct_status_type = vp->vp_integer;

	switch (acct_status_type) {
        case PW_STATUS_START:
        case PW_STATUS_ALIVE:
        case PW_STATUS_STOP:
        case PW_STATUS_ACCOUNTING_ON:
        case PW_STATUS_ACCOUNTING_OFF:
		break;

        default:
		/* We don't care about any other accounting packet */
		return RLM_MODULE_NOOP;
	}


	dissocket = data->redis_inst->redis_get_socket(data->redis_inst);
	if (dissocket == NULL) {
		RDEBUG("cannot allocate redis connection");
		return RLM_MODULE_FAIL;
	}

	switch (acct_status_type) {
        case PW_STATUS_START:
        REDIS_COMMAND(data->allocate_update, dissocket, data, request);
        (data->redis_inst->redis_finish_query)(dissocket);
		rcode = RLM_MODULE_OK;
		break;

        case PW_STATUS_ALIVE:
		REDIS_COMMAND(data->allocate_update, dissocket, data, request);
		(data->redis_inst->redis_finish_query)(dissocket);
		rcode = RLM_MODULE_OK;
		break;

        case PW_STATUS_STOP:
		REDIS_COMMAND(data->deallocate, dissocket, data, request);
		(data->redis_inst->redis_finish_query)(dissocket);
		rcode = RLM_MODULE_OK;
		break;

        case PW_STATUS_ACCOUNTING_ON:
        case PW_STATUS_ACCOUNTING_OFF:
		/* TODO */
		break;

	}

	data->redis_inst->redis_release_socket(data->redis_inst, dissocket);

	return rcode;
}


/*
 *	Only free memory we allocated.  The strings allocated via
 *	cf_section_parse() do not need to be freed.
 */
static int redis_ippool_detach(UNUSED void *instance)
{
	rlm_redis_ippool_t *inst;

	inst = instance;
	free(inst);

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_redis_ippool = {
	RLM_MODULE_INIT,
	"redis_ippool",
	RLM_TYPE_THREAD_SAFE,		/* type */
	redis_ippool_instantiate,		/* instantiation */
	redis_ippool_detach,			/* detach */
	{
		NULL,			/* authentication */
		NULL,		 	/* authorization */
		NULL,			/* preaccounting */
		redis_ippool_accounting,	/* accounting */
		NULL,			/* checksimul */
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		redis_ippool_post_auth		/* post-auth */
	},
};
