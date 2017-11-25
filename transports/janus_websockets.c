/*! \file   janus_websockets.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus WebSockets transport plugin
 * \details  This is an implementation of a WebSockets transport for the
 * Janus API, using the libwebsockets library (http://libwebsockets.org).
 * This means that, with the help of this module, browsers or applications
 * (e.g., nodejs server side implementations) can also make use of
 * WebSockets to make requests to the gateway. In that case, the same
 * WebSocket can be used for both sending requests and receiving
 * notifications, without the need for long polls. At the same time,
 * without the concept of a REST path, requests sent through the
 * WebSockets interface will need to include, when needed, additional
 * pieces of information like \c session_id and \c handle_id. That is,
 * where you'd send a Janus request related to a specific session to the
 * \c /janus/<session> path, with WebSockets you'd have to send the same
 * request with an additional \c session_id field in the JSON payload.
 * The same applies for the handle. The JavaScript library (janus.js)
 * implements all of this on the client side automatically.
 * \note When you create a session using WebSockets, a subscription to
 * the events related to it is done automatically, so no need for an
 * explicit request as the GET in the plain HTTP API. Closing a WebSocket
 * will also destroy all the sessions it created.
 *
 * \ingroup transports
 * \ref transports
 */

#include "transport.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <libwebsockets.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../utils.h"

#include "dyad.h" //Socket Library to Communicate With Cars


/* Transport plugin information */
#define JANUS_WEBSOCKETS_VERSION			1
#define JANUS_WEBSOCKETS_VERSION_STRING		"0.0.1"
#define JANUS_WEBSOCKETS_DESCRIPTION		"This transport plugin adds WebSockets support to the Janus API via libwebsockets."
#define JANUS_WEBSOCKETS_NAME				"JANUS WebSockets transport plugin"
#define JANUS_WEBSOCKETS_AUTHOR				"Meetecho s.r.l."
#define JANUS_WEBSOCKETS_PACKAGE			"janus.transport.websockets"

/* Transport methods */
janus_transport *create(void);
int janus_websockets_init(janus_transport_callbacks *callback, const char *config_path);
void janus_websockets_destroy(void);
int janus_websockets_get_api_compatibility(void);
int janus_websockets_get_version(void);
const char *janus_websockets_get_version_string(void);
const char *janus_websockets_get_description(void);
const char *janus_websockets_get_name(void);
const char *janus_websockets_get_author(void);
const char *janus_websockets_get_package(void);
gboolean janus_websockets_is_janus_api_enabled(void);
gboolean janus_websockets_is_admin_api_enabled(void);
int janus_websockets_send_message(void *transport, void *request_id, gboolean admin, json_t *message);
void janus_websockets_session_created(void *transport, guint64 session_id);
void janus_websockets_session_over(void *transport, guint64 session_id, gboolean timeout);


/* Transport setup */
static janus_transport janus_websockets_transport =
	JANUS_TRANSPORT_INIT (
		.init = janus_websockets_init,
		.destroy = janus_websockets_destroy,

		.get_api_compatibility = janus_websockets_get_api_compatibility,
		.get_version = janus_websockets_get_version,
		.get_version_string = janus_websockets_get_version_string,
		.get_description = janus_websockets_get_description,
		.get_name = janus_websockets_get_name,
		.get_author = janus_websockets_get_author,
		.get_package = janus_websockets_get_package,

		.is_janus_api_enabled = janus_websockets_is_janus_api_enabled,
		.is_admin_api_enabled = janus_websockets_is_admin_api_enabled,

		.send_message = janus_websockets_send_message,
		.session_created = janus_websockets_session_created,
		.session_over = janus_websockets_session_over,
	);

/* Transport creator */
janus_transport *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_WEBSOCKETS_NAME);
	return &janus_websockets_transport;
}


/* Useful stuff */
static gint initialized = 0, stopping = 0;
static janus_transport_callbacks *gateway = NULL;
static gboolean ws_janus_api_enabled = FALSE;
static gboolean ws_admin_api_enabled = FALSE;
static gboolean notify_events = TRUE;

/* JSON serialization options */
static size_t json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;


/* Logging */
static int ws_log_level = 0;

/* WebSockets service thread */
static GThread *ws_thread = NULL;
static GThread *socket_server_thread = NULL;
void *janus_websockets_thread(void *data);
void *tracer_socket_server_thread(void *data);


/* WebSocket client session */
typedef struct janus_websockets_client {
	struct lws *wsi;						/* The libwebsockets client instance */
	GAsyncQueue *messages;					/* Queue of outgoing messages to push */
	char *incoming;							/* Buffer containing the incoming message to process (in case there are fragments) */
	unsigned char *buffer;					/* Buffer containing the message to send */
	int buflen;								/* Length of the buffer (may be resized after re-allocations) */
	int bufpending;							/* Data an interrupted previous write couldn't send */
	int bufoffset;							/* Offset from where the interrupted previous write should resume */
	janus_mutex mutex;						/* Mutex to lock/unlock this session */
	gint session_timeout:1;					/* Whether a Janus session timeout occurred in the core */
	gint destroy:1;							/* Flag to trigger a lazy session destruction */
} janus_websockets_client;


/* libwebsockets WS context */
static struct lws_context *wsc = NULL;
/* libwebsockets sessions that have been closed */
static GList *old_wss;
static janus_mutex old_wss_mutex = JANUS_MUTEX_INITIALIZER;
/* Callbacks for HTTP-related events (automatically rejected) */
static int janus_websockets_callback_http(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
static int janus_websockets_callback_https(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
/* Callbacks for WebSockets-related events */
static int janus_websockets_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
static int janus_websockets_callback_secure(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
static int janus_websockets_admin_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
static int janus_websockets_admin_callback_secure(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len);
/* Protocol mappings */
static struct lws_protocols ws_protocols[] = {
	{ "http-only", janus_websockets_callback_http, 0, 0 },
	{ "janus-protocol", janus_websockets_callback, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols sws_protocols[] = {
	{ "http-only", janus_websockets_callback_https, 0, 0 },
	{ "janus-protocol", janus_websockets_callback_secure, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols admin_ws_protocols[] = {
	{ "http-only", janus_websockets_callback_http, 0, 0 },
	{ "janus-admin-protocol", janus_websockets_admin_callback, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols admin_sws_protocols[] = {
	{ "http-only", janus_websockets_callback_https, 0, 0 },
	{ "janus-admin-protocol", janus_websockets_admin_callback_secure, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
/* Helper for debugging reasons */
#define CASE_STR(name) case name: return #name
static const char *janus_websockets_reason_string(enum lws_callback_reasons reason) {
	switch(reason) {
		CASE_STR(LWS_CALLBACK_ESTABLISHED);
		CASE_STR(LWS_CALLBACK_CLIENT_CONNECTION_ERROR);
		CASE_STR(LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH);
		CASE_STR(LWS_CALLBACK_CLIENT_ESTABLISHED);
		CASE_STR(LWS_CALLBACK_CLOSED);
		CASE_STR(LWS_CALLBACK_CLOSED_HTTP);
		CASE_STR(LWS_CALLBACK_RECEIVE);
		CASE_STR(LWS_CALLBACK_CLIENT_RECEIVE);
		CASE_STR(LWS_CALLBACK_CLIENT_RECEIVE_PONG);
		CASE_STR(LWS_CALLBACK_CLIENT_WRITEABLE);
		CASE_STR(LWS_CALLBACK_SERVER_WRITEABLE);
		CASE_STR(LWS_CALLBACK_HTTP);
		CASE_STR(LWS_CALLBACK_HTTP_BODY);
		CASE_STR(LWS_CALLBACK_HTTP_BODY_COMPLETION);
		CASE_STR(LWS_CALLBACK_HTTP_FILE_COMPLETION);
		CASE_STR(LWS_CALLBACK_HTTP_WRITEABLE);
		CASE_STR(LWS_CALLBACK_FILTER_NETWORK_CONNECTION);
		CASE_STR(LWS_CALLBACK_FILTER_HTTP_CONNECTION);
		CASE_STR(LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED);
		CASE_STR(LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION);
		CASE_STR(LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS);
		CASE_STR(LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS);
		CASE_STR(LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION);
		CASE_STR(LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER);
		CASE_STR(LWS_CALLBACK_CONFIRM_EXTENSION_OKAY);
		CASE_STR(LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED);
		CASE_STR(LWS_CALLBACK_PROTOCOL_INIT);
		CASE_STR(LWS_CALLBACK_PROTOCOL_DESTROY);
		CASE_STR(LWS_CALLBACK_WSI_CREATE);
		CASE_STR(LWS_CALLBACK_WSI_DESTROY);
		CASE_STR(LWS_CALLBACK_GET_THREAD_ID);
		CASE_STR(LWS_CALLBACK_ADD_POLL_FD);
		CASE_STR(LWS_CALLBACK_DEL_POLL_FD);
		CASE_STR(LWS_CALLBACK_CHANGE_MODE_POLL_FD);
		CASE_STR(LWS_CALLBACK_LOCK_POLL);
		CASE_STR(LWS_CALLBACK_UNLOCK_POLL);
		CASE_STR(LWS_CALLBACK_OPENSSL_CONTEXT_REQUIRES_PRIVATE_KEY);
		CASE_STR(LWS_CALLBACK_USER);
		CASE_STR(LWS_CALLBACK_RECEIVE_PONG);
		default:
			break;
	}
	return NULL;
}



//Tracer variables
static guint64 tracer_session_id;
static GList *tracer_car_list;
static GList *tracer_driver_list;
typedef struct tracer_car {
	char *id;
	gint streamId;
	dyad_Stream *socketStream;
	janus_mutex mutex;						/* Mutex to lock/unlock this session */
}tracer_car;

typedef struct tracer_driver {
	char *id;
	struct tracer_car *carControlled; //The car driver is controlling.
	struct tracer_car *carStreamed; //The car driver is streaming.
	gboolean isVerified; //Set true if the id that driver sends over datachannel matches the id web server gave. If not verified, do not stream or let control the car.
	guint64 handleId;
	janus_mutex mutex;						/* Mutex to lock/unlock this session */
}tracer_driver;

janus_websockets_client *webserver_ws_client = NULL; //The only connection coming from the web server.



//Tracer Method Definitions

static void tracer_cut_all_controls(void);
static void tracer_stop_all_streams(void);
tracer_car* tracer_find_car(char* carid);
tracer_driver* tracer_find_driver(char* driverid);
void tracer_remove_driver(char* driverid);
tracer_driver* tracer_find_driver_by_handle_id(guint64 handleId);
static void tracer_socket_onData(dyad_Event *e);
static void tracer_socket_onClose(dyad_Event *e);
static void tracer_socket_onAccept(dyad_Event *e);
static void tracer_socket_sendMessage(dyad_Stream *socketStream, char* message);

//Tracer helper methods
static void tracer_cut_all_controls(void) {
	tracer_driver* driver = NULL;
	GList* tmp_driver_list = g_list_first(tracer_driver_list);
	while (tmp_driver_list != NULL)
	{
		driver = (tracer_driver*)tmp_driver_list->data;
		driver->carControlled = NULL;
		tmp_driver_list = tmp_driver_list->next;
	}
	g_free(tmp_driver_list);
	driver = NULL;
}

static void tracer_stop_all_streams(void) {
	tracer_driver* driver = NULL;
	GList* tmp_driver_list = g_list_first(tracer_driver_list);
	while (tmp_driver_list != NULL)
	{
		driver = (tracer_driver*)tmp_driver_list->data;
		if (driver->carStreamed != NULL)
		{
			//Send stop message to plugin
			json_t *message = json_object();
			json_object_set_new(message, "janus", json_string("message"));
			json_object_set_new(message, "opaque_id", json_string(driver->id));
			json_object_set_new(message, "transaction", json_string(driver->id));
			json_object_set_new(message, "session_id", json_integer(tracer_session_id));
			json_object_set_new(message, "handle_id", json_integer(driver->handleId));
			json_t *body = json_object();
			json_object_set_new(body, "request", json_string("stop"));
			json_object_set_new(message, "body", body);
			gateway->incoming_request(&janus_websockets_transport, webserver_ws_client, NULL, FALSE, message, NULL);
		}
		tmp_driver_list = tmp_driver_list->next;
	}
	g_free(tmp_driver_list);
	driver = NULL;
}

tracer_car* tracer_find_car(char* carid)
{
	//Find the car with the given ID
	tracer_car* car = NULL;
	GList* tmp_car_list = g_list_first(tracer_car_list);
	while (tmp_car_list != NULL)
	{
		car = (tracer_car*)tmp_car_list->data;
		if (car->id == carid)
		{
			break;
		}
		tmp_car_list = tmp_car_list->next;
	}
	g_free(tmp_car_list);
	return car;
}

tracer_driver* tracer_find_driver(char* driverid)
{
	//Find the driver with the given ID
	tracer_driver* driver = NULL;
	GList* tmp_driver_list = g_list_first(tracer_driver_list);
	while (tmp_driver_list != NULL)
	{
		driver = (tracer_driver*)tmp_driver_list->data;
		if (driver->id == driverid)
		{
			break;
		}
		tmp_driver_list = tmp_driver_list->next;
	}
	g_free(tmp_driver_list);
	return driver;
}

void tracer_remove_driver(char* driverid)
{
	//Find the driver with the given ID
	tracer_driver* driver = NULL;
	GList* tmp_driver_list = g_list_first(tracer_driver_list);
	while (tmp_driver_list != NULL)
	{
		driver = (tracer_driver*)tmp_driver_list->data;
		if (driver->id == driverid)
		{
            tracer_driver_list = g_list_remove(tracer_driver_list, tmp_driver_list->data);
            g_list_free(tmp_driver_list->data);
			break;
		}
		tmp_driver_list = tmp_driver_list->next;
	}
	g_free(tmp_driver_list);
}

tracer_driver* tracer_find_driver_by_handle_id(guint64 handleId)
{
	//Find the driver with the given ID
	tracer_driver* driver = NULL;
	GList* tmp_driver_list = g_list_first(tracer_driver_list);
	while (tmp_driver_list != NULL)
	{
		driver = (tracer_driver*)tmp_driver_list->data;
		if (driver->handleId == handleId)
		{
			break;
		}
		tmp_driver_list = tmp_driver_list->next;
	}
	g_free(tmp_driver_list);
	return driver;
}

//Tracer Socket functions for car connection

static void tracer_socket_onData(dyad_Event *e) {
    //For now, Cars only sends their IDs and nothing else. So nothing to check here.
    //Get ID of the car
    //Car is in e.udata
    tracer_car* car = (tracer_car*)e->udata;
    car->id = g_malloc0(e->size);
    memcpy(car->id, e->data, e->size);
}


static void tracer_socket_onClose(dyad_Event *e) {
    //Remove this car from the car list.
    //Car is in e.udata
    struct tracer_car *car = (struct tracer_car*)e->udata;
    tracer_car_list = g_list_remove(tracer_car_list, car);
    //TODO: Notify Web Server
}

static void tracer_socket_onAccept(dyad_Event *e) {
    //Add new Car to the car list with this socket. Put Car pointer as udata.

    struct tracer_car *new_tracer_car = g_malloc0(sizeof(tracer_car));
    janus_mutex_init(&new_tracer_car->mutex);
    new_tracer_car->socketStream = e->remote;
    tracer_car_list = g_list_append(tracer_car_list, new_tracer_car);

    dyad_addListener(e->remote, DYAD_EVENT_DATA, tracer_socket_onData, new_tracer_car);
    dyad_addListener(e->remote, DYAD_EVENT_CLOSE, tracer_socket_onClose, new_tracer_car);
    JANUS_LOG(LOG_INFO, "New car connection accepted\n");
}

static void tracer_socket_sendMessage(dyad_Stream *socketStream, char* message) {
    JANUS_LOG(LOG_INFO, "Sending message to car -> %s\n", message);
    dyad_writef(socketStream, message);
}

void *tracer_socket_server_thread(void *data) {
    JANUS_LOG(LOG_INFO, "Socket Server thread started\n");

    dyad_init();

    dyad_Stream *serv = dyad_newStream();
    dyad_addListener(serv, DYAD_EVENT_ACCEPT, tracer_socket_onAccept, NULL);
    //TODO: Get port from config file or web server
    dyad_listen(serv, 8000);
    JANUS_LOG(LOG_INFO, "Socket Server listening at 8000\n");

    while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
        dyad_update();
    }

    dyad_shutdown();

    JANUS_LOG(LOG_INFO, "Socket Server thread ended\n");
    return NULL;
}


/* Helper method to return the interface associated with a local IP address */
static char *janus_websockets_get_interface_name(const char *ip) {
	struct ifaddrs *addrs = NULL, *iap = NULL;
	getifaddrs(&addrs);
	for(iap = addrs; iap != NULL; iap = iap->ifa_next) {
		if(iap->ifa_addr && (iap->ifa_flags & IFF_UP)) {
			if(iap->ifa_addr->sa_family == AF_INET) {
				struct sockaddr_in *sa = (struct sockaddr_in *)(iap->ifa_addr);
				char buffer[16];
				inet_ntop(iap->ifa_addr->sa_family, (void *)&(sa->sin_addr), buffer, sizeof(buffer));
				if(!strcmp(ip, buffer))
					return g_strdup(iap->ifa_name);
			} else if(iap->ifa_addr->sa_family == AF_INET6) {
				struct sockaddr_in6 *sa = (struct sockaddr_in6 *)(iap->ifa_addr);
				char buffer[48];
				inet_ntop(iap->ifa_addr->sa_family, (void *)&(sa->sin6_addr), buffer, sizeof(buffer));
				if(!strcmp(ip, buffer))
					return g_strdup(iap->ifa_name);
			}
		}
	}
	freeifaddrs(addrs);
	return NULL;
}

/* WebSockets ACL list for both Janus and Admin API */
GList *janus_websockets_access_list = NULL, *janus_websockets_admin_access_list = NULL;
janus_mutex access_list_mutex;
static void janus_websockets_allow_address(const char *ip, gboolean admin) {
	if(ip == NULL)
		return;
	/* Is this an IP or an interface? */
	janus_mutex_lock(&access_list_mutex);
	if(!admin)
		janus_websockets_access_list = g_list_append(janus_websockets_access_list, (gpointer)ip);
	else
		janus_websockets_admin_access_list = g_list_append(janus_websockets_admin_access_list, (gpointer)ip);
	janus_mutex_unlock(&access_list_mutex);
}
static gboolean janus_websockets_is_allowed(const char *ip, gboolean admin) {
	JANUS_LOG(LOG_VERB, "Checking if %s is allowed to contact %s interface\n", ip, admin ? "admin" : "janus");
	if(ip == NULL)
		return FALSE;
	if(!admin && janus_websockets_access_list == NULL) {
		JANUS_LOG(LOG_VERB, "Yep\n");
		return TRUE;
	}
	if(admin && janus_websockets_admin_access_list == NULL) {
		JANUS_LOG(LOG_VERB, "Yeah\n");
		return TRUE;
	}
	janus_mutex_lock(&access_list_mutex);
	GList *temp = admin ? janus_websockets_admin_access_list : janus_websockets_access_list;
	while(temp) {
		const char *allowed = (const char *)temp->data;
		if(allowed != NULL && strstr(ip, allowed)) {
			janus_mutex_unlock(&access_list_mutex);
			return TRUE;
		}
		temp = temp->next;
	}
	janus_mutex_unlock(&access_list_mutex);
	JANUS_LOG(LOG_VERB, "Nope...\n");
	return FALSE;
}

/* Transport implementation */
int janus_websockets_init(janus_transport_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;

	/* Prepare the common context */
	struct lws_context_creation_info wscinfo;
	memset(&wscinfo, 0, sizeof wscinfo);
	wscinfo.options |= LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	/* We use vhosts on the same context to address both APIs, secure or not */
	struct lws_vhost *wss = NULL, *swss = NULL,
		*admin_wss = NULL, *admin_swss = NULL;

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_WEBSOCKETS_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL) {
		janus_config_print(config);

		/* Handle configuration */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "json");
		if(item && item->value) {
			/* Check how we need to format/serialize the JSON output */
			if(!strcasecmp(item->value, "indented")) {
				/* Default: indented, we use three spaces for that */
				json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;
			} else if(!strcasecmp(item->value, "plain")) {
				/* Not indented and no new lines, but still readable */
				json_format = JSON_INDENT(0) | JSON_PRESERVE_ORDER;
			} else if(!strcasecmp(item->value, "compact")) {
				/* Compact, so no spaces between separators */
				json_format = JSON_COMPACT | JSON_PRESERVE_ORDER;
			} else {
				JANUS_LOG(LOG_WARN, "Unsupported JSON format option '%s', using default (indented)\n", item->value);
				json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;
			}
		}

		/* Check if we need to send events to handlers */
		janus_config_item *events = janus_config_get_item_drilldown(config, "general", "events");
		if(events != NULL && events->value != NULL)
			notify_events = janus_is_true(events->value);
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_WEBSOCKETS_NAME);
		}

		item = janus_config_get_item_drilldown(config, "general", "ws_logging");
		if(item && item->value) {
			ws_log_level = atoi(item->value);
			if(ws_log_level < 0)
				ws_log_level = 0;
		}
		JANUS_LOG(LOG_VERB, "libwebsockets logging: %d\n", ws_log_level);
		lws_set_log_level(ws_log_level, NULL);
		old_wss = NULL;

		/* Any ACL for either the Janus or Admin API? */
		item = janus_config_get_item_drilldown(config, "general", "ws_acl");
		if(item && item->value) {
			gchar **list = g_strsplit(item->value, ",", -1);
			gchar *index = list[0];
			if(index != NULL) {
				int i=0;
				while(index != NULL) {
					if(strlen(index) > 0) {
						JANUS_LOG(LOG_INFO, "Adding '%s' to the Janus API allowed list...\n", index);
						janus_websockets_allow_address(g_strdup(index), FALSE);
					}
					i++;
					index = list[i];
				}
			}
			g_strfreev(list);
			list = NULL;
		}
		item = janus_config_get_item_drilldown(config, "admin", "admin_ws_acl");
		if(item && item->value) {
			gchar **list = g_strsplit(item->value, ",", -1);
			gchar *index = list[0];
			if(index != NULL) {
				int i=0;
				while(index != NULL) {
					if(strlen(index) > 0) {
						JANUS_LOG(LOG_INFO, "Adding '%s' to the Admin/monitor allowed list...\n", index);
						janus_websockets_allow_address(g_strdup(index), TRUE);
					}
					i++;
					index = list[i];
				}
			}
			g_strfreev(list);
			list = NULL;
		}

		/* Check if we need to enable the transport level ping/pong mechanism */
		int pingpong_trigger = 0, pingpong_timeout = 0;
		item = janus_config_get_item_drilldown(config, "general", "pingpong_trigger");
		if(item && item->value) {
#if LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 1
			pingpong_trigger = atoi(item->value);
			if(pingpong_trigger < 0) {
				JANUS_LOG(LOG_WARN, "Invalid value for pingpong_trigger (%d), ignoring...\n", pingpong_trigger);
				pingpong_trigger = 0;
			}
#else
			JANUS_LOG(LOG_WARN, "WebSockets ping/pong only supported in libwebsockets >= 2.1\n");
#endif
		}
		item = janus_config_get_item_drilldown(config, "general", "pingpong_timeout");
		if(item && item->value) {
#if LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 1
			pingpong_timeout = atoi(item->value);
			if(pingpong_timeout < 0) {
				JANUS_LOG(LOG_WARN, "Invalid value for pingpong_timeout (%d), ignoring...\n", pingpong_timeout);
				pingpong_timeout = 0;
			}
#else
			JANUS_LOG(LOG_WARN, "WebSockets ping/pong only supported in libwebsockets >= 2.1\n");
#endif
		}
		if((pingpong_trigger && !pingpong_timeout) || (!pingpong_trigger && pingpong_timeout)) {
			JANUS_LOG(LOG_WARN, "pingpong_trigger and pingpong_timeout not both set, ignoring...\n");
		}
#if LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 1
		if(pingpong_trigger > 0 && pingpong_timeout > 0) {
			wscinfo.ws_ping_pong_interval = pingpong_trigger;
			wscinfo.timeout_secs = pingpong_timeout;
		}
#endif

		/* Create the base context */
		wsc = lws_create_context(&wscinfo);
		if(wsc == NULL) {
			JANUS_LOG(LOG_ERR, "Error creating libwebsockets context...\n");
			janus_config_destroy(config);
			return -1;	/* No point in keeping the plugin loaded */
		}

		/* Setup the Janus API WebSockets server(s) */
		item = janus_config_get_item_drilldown(config, "general", "ws");
		if(!item || !item->value || !janus_is_true(item->value)) {
			JANUS_LOG(LOG_WARN, "WebSockets server disabled\n");
		} else {
			int wsport = 8188;
			item = janus_config_get_item_drilldown(config, "general", "ws_port");
			if(item && item->value)
				wsport = atoi(item->value);
			char *interface = NULL;
			item = janus_config_get_item_drilldown(config, "general", "ws_interface");
			if(item && item->value)
				interface = (char *)item->value;
			char *ip = NULL;
			item = janus_config_get_item_drilldown(config, "general", "ws_ip");
			if(item && item->value) {
				ip = (char *)item->value;
				char *iface = janus_websockets_get_interface_name(ip);
				if(iface == NULL) {
					JANUS_LOG(LOG_WARN, "No interface associated with %s? Falling back to no interface...\n", ip);
				}
				ip = iface;
			}
			/* Prepare context */
			struct lws_context_creation_info info;
			memset(&info, 0, sizeof info);
			info.port = wsport;
			info.iface = ip ? ip : interface;
			info.protocols = ws_protocols;
			info.extensions = NULL;
			info.ssl_cert_filepath = NULL;
			info.ssl_private_key_filepath = NULL;
			info.gid = -1;
			info.uid = -1;
			info.options = 0;
			/* Create the WebSocket context */
			wss = lws_create_vhost(wsc, &info);
			if(wss == NULL) {
				JANUS_LOG(LOG_FATAL, "Error initializing libwebsockets...\n");
			} else {
				JANUS_LOG(LOG_INFO, "WebSockets server started (port %d)...\n", wsport);
			}
			g_free(ip);
		}
		item = janus_config_get_item_drilldown(config, "general", "wss");
		if(!item || !item->value || !janus_is_true(item->value)) {
			JANUS_LOG(LOG_WARN, "Secure WebSockets server disabled\n");
		} else {
			int wsport = 8989;
			item = janus_config_get_item_drilldown(config, "general", "wss_port");
			if(item && item->value)
				wsport = atoi(item->value);
			char *interface = NULL;
			item = janus_config_get_item_drilldown(config, "general", "wss_interface");
			if(item && item->value)
				interface = (char *)item->value;
			char *ip = NULL;
			item = janus_config_get_item_drilldown(config, "general", "wss_ip");
			if(item && item->value) {
				ip = (char *)item->value;
				char *iface = janus_websockets_get_interface_name(ip);
				if(iface == NULL) {
					JANUS_LOG(LOG_WARN, "No interface associated with %s? Falling back to no interface...\n", ip);
				}
				ip = iface;
			}
			item = janus_config_get_item_drilldown(config, "certificates", "cert_pem");
			if(!item || !item->value) {
				JANUS_LOG(LOG_FATAL, "Missing certificate/key path\n");
			} else {
				char *server_pem = (char *)item->value;
				char *server_key = (char *)item->value;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_key");
				if(item && item->value)
					server_key = (char *)item->value;
				JANUS_LOG(LOG_VERB, "Using certificates:\n\t%s\n\t%s\n", server_pem, server_key);
				/* Prepare secure context */
				struct lws_context_creation_info info;
				memset(&info, 0, sizeof info);
				info.port = wsport;
				info.iface = ip ? ip : interface;
				info.protocols = sws_protocols;
				info.extensions = NULL;
				info.ssl_cert_filepath = server_pem;
				info.ssl_private_key_filepath = server_key;
				info.gid = -1;
				info.uid = -1;
#if LWS_LIBRARY_VERSION_MAJOR >= 2
				info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#else
				info.options = 0;
#endif
				/* Create the secure WebSocket context */
				swss = lws_create_vhost(wsc, &info);
				if(swss == NULL) {
					JANUS_LOG(LOG_FATAL, "Error initializing libwebsockets...\n");
				} else {
					JANUS_LOG(LOG_INFO, "Secure WebSockets server started (port %d)...\n", wsport);
				}
				g_free(ip);
			}
		}
		/* Do the same for the Admin API, if enabled */
		item = janus_config_get_item_drilldown(config, "admin", "admin_ws");
		if(!item || !item->value || !janus_is_true(item->value)) {
			JANUS_LOG(LOG_WARN, "Admin WebSockets server disabled\n");
		} else {
			int wsport = 7188;
			item = janus_config_get_item_drilldown(config, "admin", "admin_ws_port");
			if(item && item->value)
				wsport = atoi(item->value);
			char *interface = NULL;
			item = janus_config_get_item_drilldown(config, "admin", "admin_ws_interface");
			if(item && item->value)
				interface = (char *)item->value;
			char *ip = NULL;
			item = janus_config_get_item_drilldown(config, "admin", "admin_ws_ip");
			if(item && item->value) {
				ip = (char *)item->value;
				char *iface = janus_websockets_get_interface_name(ip);
				if(iface == NULL) {
					JANUS_LOG(LOG_WARN, "No interface associated with %s? Falling back to no interface...\n", ip);
				}
				ip = iface;
			}
			/* Prepare context */
			struct lws_context_creation_info info;
			memset(&info, 0, sizeof info);
			info.port = wsport;
			info.iface = ip ? ip : interface;
			info.protocols = admin_ws_protocols;
			info.extensions = NULL;
			info.ssl_cert_filepath = NULL;
			info.ssl_private_key_filepath = NULL;
			info.gid = -1;
			info.uid = -1;
			info.options = 0;
			/* Create the WebSocket context */
			admin_wss = lws_create_vhost(wsc, &info);
			if(admin_wss == NULL) {
				JANUS_LOG(LOG_FATAL, "Error initializing libwebsockets...\n");
			} else {
				JANUS_LOG(LOG_INFO, "Admin WebSockets server started (port %d)...\n", wsport);
			}
			g_free(ip);
		}
		item = janus_config_get_item_drilldown(config, "admin", "admin_wss");
		if(!item || !item->value || !janus_is_true(item->value)) {
			JANUS_LOG(LOG_WARN, "Secure Admin WebSockets server disabled\n");
		} else {
			int wsport = 7989;
			item = janus_config_get_item_drilldown(config, "admin", "admin_wss_port");
			if(item && item->value)
				wsport = atoi(item->value);
			char *interface = NULL;
			item = janus_config_get_item_drilldown(config, "admin", "admin_wss_interface");
			if(item && item->value)
				interface = (char *)item->value;
			char *ip = NULL;
			item = janus_config_get_item_drilldown(config, "admin", "admin_wss_ip");
			if(item && item->value) {
				ip = (char *)item->value;
				char *iface = janus_websockets_get_interface_name(ip);
				if(iface == NULL) {
					JANUS_LOG(LOG_WARN, "No interface associated with %s? Falling back to no interface...\n", ip);
				}
				ip = iface;
			}
			item = janus_config_get_item_drilldown(config, "certificates", "cert_pem");
			if(!item || !item->value) {
				JANUS_LOG(LOG_FATAL, "Missing certificate/key path\n");
			} else {
				char *server_pem = (char *)item->value;
				char *server_key = (char *)item->value;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_key");
				if(item && item->value)
					server_key = (char *)item->value;
				JANUS_LOG(LOG_VERB, "Using certificates:\n\t%s\n\t%s\n", server_pem, server_key);
				/* Prepare secure context */
				struct lws_context_creation_info info;
				memset(&info, 0, sizeof info);
				info.port = wsport;
				info.iface = ip ? ip : interface;
				info.protocols = admin_sws_protocols;
				info.extensions = NULL;
				info.ssl_cert_filepath = server_pem;
				info.ssl_private_key_filepath = server_key;
				info.gid = -1;
				info.uid = -1;
#if LWS_LIBRARY_VERSION_MAJOR >= 2
				info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#else
				info.options = 0;
#endif
				/* Create the secure WebSocket context */
				admin_swss = lws_create_vhost(wsc, &info);
				if(admin_swss == NULL) {
					JANUS_LOG(LOG_FATAL, "Error initializing libwebsockets...\n");
				} else {
					JANUS_LOG(LOG_INFO, "Secure Admin WebSockets server started (port %d)...\n", wsport);
				}
				g_free(ip);
			}
		}
	}
	janus_config_destroy(config);
	config = NULL;
	if(!wss && !swss && !admin_wss && !admin_swss) {
		JANUS_LOG(LOG_WARN, "No WebSockets server started, giving up...\n");
		lws_context_destroy(wsc);
		return -1;	/* No point in keeping the plugin loaded */
	}
	ws_janus_api_enabled = wss || swss;
	ws_admin_api_enabled = admin_wss || admin_swss;

	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	/* Start the WebSocket service thread */
	if(ws_janus_api_enabled || ws_admin_api_enabled) {
		ws_thread = g_thread_try_new("ws thread", &janus_websockets_thread, wsc, &error);
		if(!ws_thread) {
			g_atomic_int_set(&initialized, 0);
			JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the WebSockets thread...\n", error->code, error->message ? error->message : "??");
			return -1;
		}
	}/* Start the Socket Server service thread */
	if(ws_janus_api_enabled || ws_admin_api_enabled) {
		socket_server_thread = g_thread_try_new("socket server thread", &tracer_socket_server_thread, NULL, &error);
		if(!socket_server_thread) {
			g_atomic_int_set(&initialized, 0);
			JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Socket Server thread...\n", error->code, error->message ? error->message : "??");
			return -1;
		}
	}

	/* Done */
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_WEBSOCKETS_NAME);
	return 0;
}

void janus_websockets_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	/* Stop the service thread */
	if(ws_thread != NULL) {
		g_thread_join(ws_thread);
		ws_thread = NULL;
	}
	/* Stop the socket server thread */
	if(socket_server_thread != NULL) {
		g_thread_join(socket_server_thread);
		socket_server_thread = NULL;
	}

	/* Destroy the context */
	if(wsc != NULL) {
		lws_context_destroy(wsc);
		wsc = NULL;
	}

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_WEBSOCKETS_NAME);
}

static void janus_websockets_destroy_client(
		janus_websockets_client *ws_client,
		struct lws *wsi,
		const char *log_prefix) {
	if(!ws_client || ws_client->destroy)
		return;
	/* Notify handlers about this transport being gone */
	if(notify_events && gateway->events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "event", json_string("disconnected"));
		gateway->notify_event(&janus_websockets_transport, ws_client, info);
	}
	/* Notify core */
	gateway->transport_gone(&janus_websockets_transport, ws_client);
	/* Mark the session as closed */
	janus_mutex_lock(&old_wss_mutex);
	old_wss = g_list_append(old_wss, ws_client);
	janus_mutex_unlock(&old_wss_mutex);
	/* Cleanup */
	janus_mutex_lock(&ws_client->mutex);
	JANUS_LOG(LOG_INFO, "[%s-%p] Destroying WebSocket client\n", log_prefix, wsi);
	ws_client->destroy = 1;
	ws_client->wsi = NULL;
	/* Remove messages queue too, if needed */
	if(ws_client->messages != NULL) {
		char *response = NULL;
		while((response = g_async_queue_try_pop(ws_client->messages)) != NULL) {
			g_free(response);
		}
		g_async_queue_unref(ws_client->messages);
	}
	/* ... and the shared buffers */
	g_free(ws_client->incoming);
	ws_client->incoming = NULL;
	g_free(ws_client->buffer);
	ws_client->buffer = NULL;
	ws_client->buflen = 0;
	ws_client->bufpending = 0;
	ws_client->bufoffset = 0;
	janus_mutex_unlock(&ws_client->mutex);
}

int janus_websockets_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_TRANSPORT_API_VERSION;
}

int janus_websockets_get_version(void) {
	return JANUS_WEBSOCKETS_VERSION;
}

const char *janus_websockets_get_version_string(void) {
	return JANUS_WEBSOCKETS_VERSION_STRING;
}

const char *janus_websockets_get_description(void) {
	return JANUS_WEBSOCKETS_DESCRIPTION;
}

const char *janus_websockets_get_name(void) {
	return JANUS_WEBSOCKETS_NAME;
}

const char *janus_websockets_get_author(void) {
	return JANUS_WEBSOCKETS_AUTHOR;
}

const char *janus_websockets_get_package(void) {
	return JANUS_WEBSOCKETS_PACKAGE;
}

gboolean janus_websockets_is_janus_api_enabled(void) {
	return ws_janus_api_enabled;
}

gboolean janus_websockets_is_admin_api_enabled(void) {
	return ws_admin_api_enabled;
}

int janus_websockets_send_message(void *transport, void *request_id, gboolean admin, json_t *message) {
	if(message == NULL)
		return -1;
	if(transport == NULL) {
		json_decref(message);
		return -1;
	}

	/* Make sure this is not related to a closed /freed WebSocket session */
	janus_mutex_lock(&old_wss_mutex);
	janus_websockets_client *client = (janus_websockets_client *)transport;
	if(g_list_find(old_wss, client) != NULL || !client->wsi) {
		json_decref(message);
		message = NULL;
		transport = NULL;
		janus_mutex_unlock(&old_wss_mutex);
		return -1;
	}
	janus_mutex_lock(&client->mutex);

	tracer_driver* driver = NULL;
	guint64 handleId = 0;
	json_t *sender = json_object_get(message, "sender");
	if (sender)
	{
		handleId = json_integer_value(sender);
		driver = tracer_find_driver_by_handle_id(handleId);
	}

	const char *janus_text = NULL;
	json_t *j = json_object_get(message, "janus");
	if (!j)
	{
		return -1;
	}
	janus_text = json_string_value(j);

	json_t *info; 
	if (!strcasecmp(janus_text, "event"))
	{
		//if plugindata -> data -> result -> status == preparing then send jsep -> sdp to web server as offer

		json_t *plugindata = json_object_get(message, "plugindata");
		if (!plugindata)
		{
			return 0;
		}
		json_t *data = json_object_get(plugindata, "data");
		if (!data)
		{
			return 0;
		}
		json_t *streaming = json_object_get(data, "streaming");
		if (!streaming)
		{
			return 0;
		}
		const char* streaming_text = json_string_value(streaming);
		if (!strcasecmp(streaming_text, "message"))
		{
			//We got message from data channel
			json_t *channelMessage = json_object_get(data, "message");
			if (!channelMessage)
			{
				return 0;
			}
			//This is the pure message got from data channel. It may be ID of the driver or control command.
			const char* channelMessage_text = json_string_value(channelMessage);
			if (channelMessage_text[0] == '1')
			{
				//This is control command. Send it to car driver controls.
				if (driver->carControlled)
				{
                    char* carCommand = (char *)channelMessage_text;
					tracer_socket_sendMessage(driver->carControlled->socketStream, carCommand);
				}
			}
			else if (channelMessage_text[0] == '0')
			{
				//This is driver id. If driver is not Verified, check if this id matches with the one web server gave us.
				if (driver->isVerified)
				{
					return 0;
				}else
				{
                    char* dId = (char *)channelMessage_text + 1; //Remove the first character to get only the ID.
					if (driver->id == dId)
					{
						driver->isVerified = TRUE;

						//Notify web server that this driver gave right ID and verified.
						info = json_object();
						json_object_set_new(info, "info", json_string("verified"));
						json_object_set_new(info, "driverid", json_string(driver->id));
					}else
					{
						//Send hangup request to plugin.
                        json_t *request = json_object();
						json_object_set_new(request, "janus", json_string("hangup"));
						json_object_set_new(request, "opaque_id", json_string(driver->id));
						json_object_set_new(request, "transaction", json_string(driver->id));
						json_object_set_new(request, "session_id", json_integer(tracer_session_id));
						json_object_set_new(request, "handle_id", json_integer(driver->handleId));

						gateway->incoming_request(&janus_websockets_transport, client, NULL, FALSE, request, NULL);

						//Notify web server that this driver gave wrong ID and disconnected.
						info = json_object();
						json_object_set_new(info, "info", json_string("wrongid"));
						json_object_set_new(info, "driverid", json_string(driver->id));
						
						//Remove driver from the list
                        tracer_remove_driver(driver->id);

					}
					goto send_info;
				}
			}


		}
		else if (!strcasecmp(streaming_text, "event"))
		{
			//We are only interested in offer the plugin sends
			json_t *jsep = json_object_get(message, "jsep");
			if (!jsep)
			{
				return 0;
			}
			json_t *sdp = json_object_get(jsep, "sdp");

			info = json_object();
			json_object_set_new(info, "info", json_string("offer"));
			json_object_set_new(info, "driverid", json_string(driver->id));
			json_object_set_new(info, "sdp", sdp);

			goto send_info;
		}


	}
	else if (!strcasecmp(janus_text, "success"))
	{
		json_t *data = json_object_get(message, "data");
		json_t *id = json_object_get(data, "id");
		if (id == NULL)
		{
			//Nothing interesting
			return 0;
		}

		json_t *sid = json_object_get(message, "session_id");
		if (sid) //If there is session_id, then it is most likely the handle_id came.
		{
			guint64 handle_id = 0;
			handle_id = json_integer_value(id);
			json_t *t = json_object_get(message, "transaction");
            const char *transaction_text = json_string_value(t);
			driver = tracer_find_driver((char *)transaction_text); //We gave driver id as transaction when attaching to the plugin.
			if (driver == NULL)
				return -1;
			driver->handleId = handle_id;

			json_decref(message);

			//Send connect message to plugin
			json_t *request = json_object();
			json_object_set_new(request, "janus", json_string("message"));
			json_object_set_new(request, "transaction", json_string(driver->id));
			json_t *body = json_object();
			json_object_set_new(body, "request", json_string("connect"));
			json_object_set_new(body, "id", json_integer(1)); //Just to get some info about mountpoints to create SDP.
			json_object_set_new(request, "body", body);
			json_object_set_new(request, "session_id", json_integer(tracer_session_id));

			gateway->incoming_request(&janus_websockets_transport, client, NULL, 0, request, NULL);
			return 0;
		}else
		{
			//We got our session_id
			tracer_session_id = json_integer_value(id);
			return 0;
		}

	} else if (!strcasecmp(janus_text, "webrtcup"))
	{
		info = json_object();
		json_object_set_new(info, "info", json_string("connected"));
		json_object_set_new(info, "driverid", json_string(driver->id));

		goto send_info;
	} 

	send_info:
    {
		//Send info to web server

		/* Convert to string and enqueue */
		char *payload = json_dumps(info, json_format);
		g_async_queue_push(client->messages, payload);
		lws_callback_on_writable(client->wsi);
		janus_mutex_unlock(&client->mutex);
		janus_mutex_unlock(&old_wss_mutex);
		json_decref(info);
		json_decref(message);
		return 0;
};

	//Look at the data coming from Janus core or Streaming Plugin

	//Commands;
		//sessioncreated: Get the session id
	/*{
		"janus": "success",
			"transaction" : "1zzCbzMFg87f",
			"data" : {
			"id": 7369322695984436
		}
	}*/
		//handle created: Get the handle id and put it to the driver.
	/*{
		"janus": "success",
			"session_id" : 7369322695984436,
			"transaction" : "wv662x7enjVe",
			"data" : {
			"id": 6525731480541582
		}
	}*/
		//offer: Send it to web server with driverId.
	/*{
		"janus": "event",
			"session_id" : 7369322695984436,
			"sender" : 6525731480541582,
			"transaction" : "uaMFkaAaq2fB",
			"plugindata" : {
			"plugin": "janus.plugin.streaming",
				"data" : {
				"streaming": "event",
					"result" : {
					"status": "preparing"
				}
			}
		},
			"jsep": {
			"type": "offer",
				"sdp" : "v=0\r\no=- 1511473077116186 1511473077116186 IN IP4 143.225.229.1\r\ns=Mountpoint 1\r\nt=0 0\r\na=group:BUNDLE audio video\r\na=msid-semantic: WMS janus\r\nm=audio 9 RTP/SAVPF 111\r\nc=IN IP4 143.225.229.1\r\na=sendonly\r\na=mid:audio\r\na=rtcp-mux\r\na=ice-ufrag:BHUa\r\na=ice-pwd:8soJX9VeRZsyZD5SiX1sla\r\na=ice-options:trickle\r\na=fingerprint:sha-256 D2:B9:31:8F:DF:24:D8:0E:ED:D2:EF:25:9E:AF:6F:B8:34:AE:53:9C:E6:F3:8F:F2:64:15:FA:E8:7F:53:2D:38\r\na=setup:actpass\r\na=rtpmap:111 opus/48000/2\r\na=ssrc:368921022 cname:janusaudio\r\na=ssrc:368921022 msid:janus janusa0\r\na=ssrc:368921022 mslabel:janus\r\na=ssrc:368921022 label:janusa0\r\na=candidate:1 1 udp 2013266431 172.18.0.2 56294 typ host\r\na=candidate:2 1 udp 2013266431 143.225.229.1 48331 typ host\r\na=end-of-candidates\r\nm=video 9 RTP/SAVPF 100\r\nc=IN IP4 143.225.229.1\r\na=sendonly\r\na=mid:video\r\na=rtcp-mux\r\na=ice-ufrag:BHUa\r\na=ice-pwd:8soJX9VeRZsyZD5SiX1sla\r\na=ice-options:trickle\r\na=fingerprint:sha-256 D2:B9:31:8F:DF:24:D8:0E:ED:D2:EF:25:9E:AF:6F:B8:34:AE:53:9C:E6:F3:8F:F2:64:15:FA:E8:7F:53:2D:38\r\na=setup:actpass\r\na=rtpmap:100 VP8/90000\r\na=rtcp-fb:100 nack\r\na=rtcp-fb:100 goog-remb\r\na=ssrc:91687253 cname:janusvideo\r\na=ssrc:91687253 msid:janus janusv0\r\na=ssrc:91687253 mslabel:janus\r\na=ssrc:91687253 label:janusv0\r\na=candidate:1 1 udp 2013266431 172.18.0.2 56294 typ host\r\na=candidate:2 1 udp 2013266431 143.225.229.1 48331 typ host\r\na=end-of-candidates\r\n"
		}
	}*/
		//Webrtcup: Let web server know that we connected to driver via webrtc. Bunu verified kisminda da yapabiliriz.
	/*{
		"janus": "webrtcup",
			"session_id" : 7369322695984436,
			"sender" : 6525731480541582
	}*/


}

void janus_websockets_session_created(void *transport, guint64 session_id) {
	/* We don't care */
}

void janus_websockets_session_over(void *transport, guint64 session_id, gboolean timeout) {
	if(transport == NULL || !timeout)
		return;
	/* We only care if it's a timeout: if so, close the connection */
	janus_websockets_client *client = (janus_websockets_client *)transport;
	/* Make sure this is not related to a closed WebSocket session */
	janus_mutex_lock(&old_wss_mutex);
	if(g_list_find(old_wss, client) == NULL && client->wsi){
		janus_mutex_lock(&client->mutex);
		client->session_timeout = 1;
		lws_callback_on_writable(client->wsi);
		janus_mutex_unlock(&client->mutex);
	}
	janus_mutex_unlock(&old_wss_mutex);
}


/* Thread */
void *janus_websockets_thread(void *data) {
	struct lws_context *service = (struct lws_context *)data;
	if(service == NULL) {
		JANUS_LOG(LOG_ERR, "Invalid service\n");
		return NULL;
	}

	JANUS_LOG(LOG_INFO, "WebSockets thread started\n");

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		/* libwebsockets is single thread, we cycle through events here */
		lws_service(service, 50);
	}

	/* Get rid of the WebSockets server */
	lws_cancel_service(service);
	/* Done */
	JANUS_LOG(LOG_INFO, "WebSockets thread ended\n");
	return NULL;
}



/* WebSockets */
static int janus_websockets_callback_http(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	/* This endpoint cannot be used for HTTP */
	switch(reason) {
		case LWS_CALLBACK_HTTP:
			JANUS_LOG(LOG_VERB, "Rejecting incoming HTTP request on WebSockets endpoint\n");
			lws_return_http_status(wsi, 403, NULL);
			/* Close and free connection */
			return -1;
		case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
			if (!in) {
				JANUS_LOG(LOG_VERB, "Rejecting incoming HTTP request on WebSockets endpoint: no sub-protocol specified\n");
				return -1;
			}
			break;
		default:
			break;
	}
	return 0;
}

static int janus_websockets_callback_https(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	/* We just forward the event to the HTTP handler */
	return janus_websockets_callback_http(wsi, reason, user, in, len);
}

/* This callback handles Janus API requests */
static int janus_websockets_common_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len, gboolean admin)
{
	const char *log_prefix = admin ? "AdminWSS" : "WSS";
	janus_websockets_client *ws_client = (janus_websockets_client *)user;

	//TODO: Her baglanti geldiginde atama yapmak yerine, mesaj geldiginde dogru sifreyle gelirse atama yap.
	webserver_ws_client = ws_client;

	switch(reason) {
		case LWS_CALLBACK_ESTABLISHED: {
			/* Is there any filtering we should apply? */
			char ip[256];
#ifdef HAVE_LIBWEBSOCKETS_PEER_SIMPLE
			lws_get_peer_simple(wsi, ip, 256);
			JANUS_LOG(LOG_VERB, "[%s-%p] WebSocket connection opened from %s\n", log_prefix, wsi, ip);
#else
			char name[256];
			lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi), name, 256, ip, 256);
			JANUS_LOG(LOG_VERB, "[%s-%p] WebSocket connection opened from %s by %s\n", log_prefix, wsi, ip, name);
#endif
			if(!janus_websockets_is_allowed(ip, admin)) {
				JANUS_LOG(LOG_ERR, "[%s-%p] IP %s is unauthorized to connect to the WebSockets %s API interface\n", log_prefix, wsi, ip, admin ? "Admin" : "Janus");
				/* Close the connection */
				lws_callback_on_writable(wsi);
				return -1;
			}
			JANUS_LOG(LOG_VERB, "[%s-%p] WebSocket connection accepted\n", log_prefix, wsi);
			if(ws_client == NULL) {
				JANUS_LOG(LOG_ERR, "[%s-%p] Invalid WebSocket client instance...\n", log_prefix, wsi);
				return -1;
			}
			/* Clean the old sessions list, in case this pointer was used before */
			janus_mutex_lock(&old_wss_mutex);
			if(g_list_find(old_wss, ws_client) != NULL)
				old_wss = g_list_remove(old_wss, ws_client);
			janus_mutex_unlock(&old_wss_mutex);
			/* Prepare the session */
			ws_client->wsi = wsi;
			ws_client->messages = g_async_queue_new();
			ws_client->buffer = NULL;
			ws_client->buflen = 0;
			ws_client->bufpending = 0;
			ws_client->bufoffset = 0;
			ws_client->session_timeout = 0;
			ws_client->destroy = 0;
			janus_mutex_init(&ws_client->mutex);
			/* Let us know when the WebSocket channel becomes writeable */
			lws_callback_on_writable(wsi);
			JANUS_LOG(LOG_VERB, "[%s-%p]   -- Ready to be used!\n", log_prefix, wsi);
			/* Notify handlers about this new transport */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("connected"));
				json_object_set_new(info, "admin_api", admin ? json_true() : json_false());
				json_object_set_new(info, "ip", json_string(ip));
				gateway->notify_event(&janus_websockets_transport, ws_client, info);
			}
			return 0;
		}
		case LWS_CALLBACK_RECEIVE: {
			JANUS_LOG(LOG_HUGE, "[%s-%p] Got %zu bytes:\n", log_prefix, wsi, len);
			if(ws_client == NULL || ws_client->wsi == NULL) {
				JANUS_LOG(LOG_ERR, "[%s-%p] Invalid WebSocket client instance...\n", log_prefix, wsi);
				return -1;
			}
			/* Is this a new message, or part of a fragmented one? */
			const size_t remaining = lws_remaining_packet_payload(wsi);
			if(ws_client->incoming == NULL) {
				JANUS_LOG(LOG_HUGE, "[%s-%p] First fragment: %zu bytes, %zu remaining\n", log_prefix, wsi, len, remaining);
				ws_client->incoming = g_malloc0(len+1);
				memcpy(ws_client->incoming, in, len);
				ws_client->incoming[len] = '\0';
				JANUS_LOG(LOG_HUGE, "%s\n", ws_client->incoming);
			} else {
				size_t offset = strlen(ws_client->incoming);
				JANUS_LOG(LOG_HUGE, "[%s-%p] Appending fragment: offset %zu, %zu bytes, %zu remaining\n", log_prefix, wsi, offset, len, remaining);
				ws_client->incoming = g_realloc(ws_client->incoming, offset+len+1);
				memcpy(ws_client->incoming+offset, in, len);
				ws_client->incoming[offset+len] = '\0';
				JANUS_LOG(LOG_HUGE, "%s\n", ws_client->incoming+offset);
			}
			if(remaining > 0 || !lws_is_final_fragment(wsi)) {
				/* Still waiting for some more fragments */
				JANUS_LOG(LOG_HUGE, "[%s-%p] Waiting for more fragments\n", log_prefix, wsi);
				return 0;
			}
			JANUS_LOG(LOG_HUGE, "[%s-%p] Done, parsing message: %zu bytes\n", log_prefix, wsi, strlen(ws_client->incoming));
			/* If we got here, the message is complete: parse the JSON payload */
			json_error_t error;
			json_t *root = json_loads(ws_client->incoming, 0, &error);
			g_free(ws_client->incoming);
			ws_client->incoming = NULL;

			const char *command_text = NULL;
			json_t *command = json_object_get(root, "command");
			if (!command)
			{
                return 0;
			}
            command_text = json_string_value(command);



			//Find the car with the given ID
			tracer_car* car = NULL;
			tracer_driver* driver = NULL;
			const char* carId = NULL;
            const char* driverId = NULL;
			json_t *carid = json_object_get(root, "carid");
			if (carid)
			{
				carId = json_string_value(carid);
				car = tracer_find_car((char *)carId);
				if (car == NULL)
				{
					//TODO: Handle this. Let web server know that there is no car with that id.

					return 0;
				}
			}
			json_t *driverid = json_object_get(root, "driverid");
			if (driverid)
			{
				driverId = json_string_value(driverid);
				driver = tracer_find_driver((char *)driverId);
				if (driver == NULL)
				{
					//TODO: Handle this. Let web server know that there is no driver with that id.

					return 0;
				}
			}
			

			json_t *message = NULL;

			if (!strcasecmp(command_text, "connecttodriver"))
			{
				//Call attach to create new handle
				//{"janus":"attach","plugin":"janus.plugin.streaming","opaque_id":"streamingtest-5bLT8TwCT2p3","transaction":"wv662x7enjVe","force-bundle":true,"force-rtcp-mux":true}
				//Prepare 'connect' command and send it to streaming plugin through gateway.
				//{"janus":"message", "body" : {"request":"watch", "id" : 2}, "transaction" : "EtKVfSvaJYrU"}

				
				struct tracer_driver *new_tracer_driver = g_malloc0(sizeof(tracer_driver));
				janus_mutex_init(&new_tracer_driver->mutex);
				new_tracer_driver->id = (char *)driverId;
				new_tracer_driver->carControlled = NULL;
				new_tracer_driver->carStreamed = NULL;
				tracer_driver_list = g_list_append(tracer_driver_list, new_tracer_driver);
				
				message = json_object();
				json_object_set_new(message, "janus", json_string("attach"));
				json_object_set_new(message, "plugin", json_string("janus.plugin.streaming"));
				json_object_set_new(message, "opaque_id", json_string(driverId));
				json_object_set_new(message, "transaction", json_string(new_tracer_driver->id));
				json_object_set_new(message, "force-bundle", json_true());
				json_object_set_new(message, "force-rtcp-mux", json_true());
				json_object_set_new(message, "session_id", json_integer(tracer_session_id));

				goto send_message;

			} else if (!strcasecmp(command_text, "disconnectdriver"))
			{
				//Send hangup message to plugin
				message = json_object();
				json_object_set_new(message, "janus", json_string("hangup"));
				json_object_set_new(message, "opaque_id", json_string(driverId));
				json_object_set_new(message, "transaction", json_string(driver->id));
				json_object_set_new(message, "session_id", json_integer(tracer_session_id));
				json_object_set_new(message, "handle_id", json_integer(driver->handleId));

				//Remove driver from the list
                tracer_remove_driver(driver->id);

				goto send_message;

			} else if (!strcasecmp(command_text, "startstream"))
			{
				//{janus: "message", body: {request: "watch", id: 1}, transaction: "d3pNxxpfMd3d"}

				//Send watch message to plugin
				message = json_object();
				json_object_set_new(message, "janus", json_string("message"));
				json_object_set_new(message, "opaque_id", json_string(driverId));
				json_object_set_new(message, "transaction", json_string(driver->id));
				json_object_set_new(message, "session_id", json_integer(tracer_session_id));
				json_object_set_new(message, "handle_id", json_integer(driver->handleId));
				json_t *body = json_object();
				json_object_set_new(body, "request", json_string("watch"));
				json_object_set_new(body, "id", json_integer(car->streamId));
				json_object_set_new(message, "body", body);

				//Set driver's streamingCar to the selected car
				driver->carStreamed = car;

				goto send_message;

			} else if (!strcasecmp(command_text, "stopstream"))
			{
				//Send stop message to the plugin
				message = json_object();
				json_object_set_new(message, "janus", json_string("message"));
				json_object_set_new(message, "opaque_id", json_string(driverId));
				json_object_set_new(message, "transaction", json_string(driver->id));
				json_object_set_new(message, "session_id", json_integer(tracer_session_id));
				json_object_set_new(message, "handle_id", json_integer(driver->handleId));
				json_t *body = json_object();
				json_object_set_new(body, "request", json_string("stop"));
				json_object_set_new(message, "body", body);
				//Set driver's streamingCar to NULL
				driver->carStreamed = NULL;

				goto send_message;

			} else if (!strcasecmp(command_text, "addcontrol"))
			{
				//Set driver's contollingCar to selected car
				driver->carControlled = car;
				return 0;
			} else if (!strcasecmp(command_text, "removecontrol"))
			{
				//Set driver's contollingCar to NULL
				driver->carControlled = NULL;
				return 0;
			} else if (!strcasecmp(command_text, "removeallcontrols"))
			{
				//For each in driver list and set their controllingCar to NULL
				tracer_cut_all_controls();
				return 0;
			} else if (!strcasecmp(command_text, "stopallstreams"))
			{
				//For each in driver list and send stop message to plugin with their handle ids
				tracer_stop_all_streams();
				return 0;
			} else if (!strcasecmp(command_text, "controlcar"))
			{
				//Send speed and steering values to the car
				const char* throttle_str = NULL;
                const char* steering_str = NULL;
				json_t *throttle = json_object_get(root, "throttle");
				if (throttle)
                    throttle_str = json_string_value(throttle);
				json_t *steering = json_object_get(root, "steering");
				if (steering)
                    steering_str = json_string_value(steering);

				char command[6] = { '1', throttle_str[0], throttle_str[1], steering_str[0], steering_str[1], '\0' };

				tracer_socket_sendMessage(car->socketStream, command);
				return 0;
			}

send_message:

				if (message != NULL)
				{
					gateway->incoming_request(&janus_websockets_transport, ws_client, NULL, FALSE, message, &error);
				}

				car = NULL;
				driver = NULL;

				//See what The Web Server is trying to tell us.
				//Commands;
					//connecttodriver: includes the id of the driver. Send a webrtc offer to web server with driverId
					//connectdriverstocars: includes ids of the drivers and the cars.
					//disconnectdrivers: includes ids of drivers
					//startstreams: Start stream of cars to drivers with given ids. If driver is not connected the car, connect them first. includes carid<->driverid list
					//stopstream: Send Stop streaming command to the car. Also stop commanding. Driver is still connected to the car.
					//addcontrol: Let driver control the car. Also start streaming.
					//removecontrol: Stop letting driver control the car.
					//removeallcontrols: Stops letting all drivers control their cars.
					//stopallstreams: Send stop stream command to all cars. Stop all controls.
					//startallstreams: Send start stream command to all cars.
					//addallcontrols: Send start stream command to all cars. Start all controls.
					//controlcar: includes carId, speed and steering values.
			
				/* Notify the core, passing both the object and, since it may be needed, the error */
				//gateway->incoming_request(&janus_websockets_transport, ws_client, NULL, admin, root, &error);
				return 0;
		}
		case LWS_CALLBACK_SERVER_WRITEABLE: {
			if(ws_client == NULL || ws_client->wsi == NULL) {
				JANUS_LOG(LOG_ERR, "[%s-%p] Invalid WebSocket client instance...\n", log_prefix, wsi);
				return -1;
			}
			if(!ws_client->destroy && !g_atomic_int_get(&stopping)) {
				janus_mutex_lock(&ws_client->mutex);
				/* Check if we have a pending/partial write to complete first */
				if(ws_client->buffer && ws_client->bufpending > 0 && ws_client->bufoffset > 0
						&& !ws_client->destroy && !g_atomic_int_get(&stopping)) {
					JANUS_LOG(LOG_HUGE, "[%s-%p] Completing pending WebSocket write (still need to write last %d bytes)...\n",
						log_prefix, wsi, ws_client->bufpending);
					int sent = lws_write(wsi, ws_client->buffer + ws_client->bufoffset, ws_client->bufpending, LWS_WRITE_TEXT);
					JANUS_LOG(LOG_HUGE, "[%s-%p]   -- Sent %d/%d bytes\n", log_prefix, wsi, sent, ws_client->bufpending);
					if(sent > -1 && sent < ws_client->bufpending) {
						/* We still couldn't send everything that was left, we'll try and complete this in the next round */
						ws_client->bufpending -= sent;
						ws_client->bufoffset += sent;
					} else {
						/* Clear the pending/partial write queue */
						ws_client->bufpending = 0;
						ws_client->bufoffset = 0;
					}
					/* Done for this round, check the next response/notification later */
					lws_callback_on_writable(wsi);
					janus_mutex_unlock(&ws_client->mutex);
					return 0;
				}
				/* Shoot all the pending messages */
				char *response = g_async_queue_try_pop(ws_client->messages);
				if(response && !ws_client->destroy && !g_atomic_int_get(&stopping)) {
					/* Gotcha! */
					int buflen = LWS_SEND_BUFFER_PRE_PADDING + strlen(response) + LWS_SEND_BUFFER_POST_PADDING;
					if(ws_client->buffer == NULL) {
						/* Let's allocate a shared buffer */
						JANUS_LOG(LOG_HUGE, "[%s-%p] Allocating %d bytes (response is %zu bytes)\n", log_prefix, wsi, buflen, strlen(response));
						ws_client->buflen = buflen;
						ws_client->buffer = g_malloc0(buflen);
					} else if(buflen > ws_client->buflen) {
						/* We need a larger shared buffer */
						JANUS_LOG(LOG_HUGE, "[%s-%p] Re-allocating to %d bytes (was %d, response is %zu bytes)\n", log_prefix, wsi, buflen, ws_client->buflen, strlen(response));
						ws_client->buflen = buflen;
						ws_client->buffer = g_realloc(ws_client->buffer, buflen);
					}
					memcpy(ws_client->buffer + LWS_SEND_BUFFER_PRE_PADDING, response, strlen(response));
					JANUS_LOG(LOG_HUGE, "[%s-%p] Sending WebSocket message (%zu bytes)...\n", log_prefix, wsi, strlen(response));
					int sent = lws_write(wsi, ws_client->buffer + LWS_SEND_BUFFER_PRE_PADDING, strlen(response), LWS_WRITE_TEXT);
					JANUS_LOG(LOG_HUGE, "[%s-%p]   -- Sent %d/%zu bytes\n", log_prefix, wsi, sent, strlen(response));
					if(sent > -1 && sent < (int)strlen(response)) {
						/* We couldn't send everything in a single write, we'll complete this in the next round */
						ws_client->bufpending = strlen(response) - sent;
						ws_client->bufoffset = LWS_SEND_BUFFER_PRE_PADDING + sent;
						JANUS_LOG(LOG_HUGE, "[%s-%p]   -- Couldn't write all bytes (%d missing), setting offset %d\n",
							log_prefix, wsi, ws_client->bufpending, ws_client->bufoffset);
					}
					/* We can get rid of the message */
					free(response);
					/* Done for this round, check the next response/notification later */
					lws_callback_on_writable(wsi);
					janus_mutex_unlock(&ws_client->mutex);
					return 0;
				}
				janus_mutex_unlock(&ws_client->mutex);
			}
			return 0;
		}
		case LWS_CALLBACK_CLOSED: {
			JANUS_LOG(LOG_VERB, "[%s-%p] WS connection down, closing\n", log_prefix, wsi);
			janus_websockets_destroy_client(ws_client, wsi, log_prefix);
			JANUS_LOG(LOG_VERB, "[%s-%p]   -- closed\n", log_prefix, wsi);
			return 0;
		}
		case LWS_CALLBACK_WSI_DESTROY: {
			JANUS_LOG(LOG_VERB, "[%s-%p] WS connection down, destroying\n", log_prefix, wsi);
			janus_websockets_destroy_client(ws_client, wsi, log_prefix);
			JANUS_LOG(LOG_VERB, "[%s-%p]   -- destroyed\n", log_prefix, wsi);
			return 0;
		}
		default:
			if(wsi != NULL) {
				JANUS_LOG(LOG_HUGE, "[%s-%p] %d (%s)\n", log_prefix, wsi, reason, janus_websockets_reason_string(reason));
			} else {
				JANUS_LOG(LOG_HUGE, "[%s] %d (%s)\n", log_prefix, reason, janus_websockets_reason_string(reason));
			}
			break;
	}
	return 0;
}




/* This callback handles Janus API requests */
static int janus_websockets_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	return janus_websockets_common_callback(wsi, reason, user, in, len, FALSE);
}

static int janus_websockets_callback_secure(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	/* We just forward the event to the Janus API handler */
	return janus_websockets_callback(wsi, reason, user, in, len);
}

/* This callback handles Admin API requests */
static int janus_websockets_admin_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	return janus_websockets_common_callback(wsi, reason, user, in, len, TRUE);
}

static int janus_websockets_admin_callback_secure(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	/* We just forward the event to the Admin API handler */
	return janus_websockets_admin_callback(wsi, reason, user, in, len);
}
