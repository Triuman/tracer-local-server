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

//export LD_LIBRARY_PATH=/usr/lib64

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

#include <math.h>
#include "dyad.h" //Socket Library to Communicate With Cars

/* Transport plugin information */
#define JANUS_WEBSOCKETS_VERSION			1
#define JANUS_WEBSOCKETS_VERSION_STRING		"0.0.1"
#define JANUS_WEBSOCKETS_DESCRIPTION		"This transport plugin adds WebSockets support to the Janus API via libwebsockets."
#define JANUS_WEBSOCKETS_NAME				"JANUS WebSockets transport plugin"
#define JANUS_WEBSOCKETS_AUTHOR				"Meetecho s.r.l."
#define JANUS_WEBSOCKETS_PACKAGE			"janus.transport.websockets"
#define JANUS_STREAMING_PACKAGE			"janus.plugin.streaming"

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
static GThread *race_thread = NULL;
void *janus_websockets_thread(void *data);
void *tracer_socket_server_thread(void *data);
void *tracer_race_thread(void *data);

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
	{ "tracer-protocol", janus_websockets_callback, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols sws_protocols[] = {
	{ "http-only", janus_websockets_callback_https, 0, 0 },
	{ "tracer-protocol", janus_websockets_callback_secure, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols admin_ws_protocols[] = {
	{ "http-only", janus_websockets_callback_http, 0, 0 },
	{ "tracer-admin-protocol", janus_websockets_admin_callback, sizeof(janus_websockets_client), 0 },
	{ NULL, NULL, 0 }
};
static struct lws_protocols admin_sws_protocols[] = {
	{ "http-only", janus_websockets_callback_https, 0, 0 },
	{ "tracer-admin-protocol", janus_websockets_admin_callback_secure, sizeof(janus_websockets_client), 0 },
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

typedef struct tracer_track
{
    char *id;
    int line1_points_size;
    int line2_points_size;
    double (*line1_points)[3]; //while(line1_points[a]){a++;}
    double (*line2_points)[3];
    janus_mutex mutex; /* Mutex to lock/unlock this session */
} tracer_track;

typedef struct tracer_race
{
    char *id;
    struct tracer_track *track;
    GList *car_list;
    struct tracer_car *car_ranking_list[4]; //We add the car to here when it finishes the race.
    int finished_car_count;                 //This is the number of cars that finished all the laps. If finished_car_count = car_count, finish the race.
    int car_count;
    int lap_count; //How many laps this race has in total.
    gint64 start_time;
    gint32 elapsed_time; //we increase this by one each second.
    int max_duration;    //in seconds. If racers do not finish the laps in max_duration time, we finish the race automatically.
    gboolean is_started;
    gboolean is_paused;
    gboolean is_finished;
    janus_mutex mutex; /* Mutex to lock/unlock this session */
} tracer_race;

typedef struct tracer_car
{
    char *id;
    gint mountpoint_id_left;
    gint mountpoint_id_right;
    dyad_Stream *socket_stream;
    gboolean is_online;      //if car disconnect, we do not remove it from the list. We set is_online FALSE and wait it to come back.
    struct tracer_driver *driver; //We use this to determine ranking of drivers by cars ranking. This is the ultimate driver who owns and controls the car. We set this driver when we get a giveControlOfCar command. Sometimes we can give stream of this car to someone else for action purposes. But this driver will be shown in the ranking list.
    double *lap_times;
    gint32 finish_time; //we set this when car finishes the race to race->elapsed_time. We use this or progress value to determine ranking.
    int lap_count;      //how many laps this car passed
    double progress;    //How much of track this car completed so far. value is 0-1 per lap.
    janus_mutex mutex;  /* Mutex to lock/unlock this session */
} tracer_car;

typedef struct tracer_driver
{
    char *id;
    struct tracer_car *car;  //The car driver is controlling and most likely also streming. The game may let a driver stream other drivers' cars.
    gboolean is_online;      //if driver disconnect, we do not remove it from the list. We set is_online FALSE and wait him to come back.
    gboolean is_controlling; //If FALSE, we dont rely driver's commands to the car.
    guint64 handle_id_left;
    guint64 handle_id_right;
    janus_mutex mutex; /* Mutex to lock/unlock this session */
} tracer_driver;

janus_websockets_client *tracer_webserver_ws_client = NULL; //The only connection coming from the web server.



//Tracer variables
static guint64 tracer_session_id;
static struct tracer_track *the_tracer_track;
static struct tracer_race *the_tracer_race;
static GList *tracer_car_list;
static GList *tracer_driver_list;
janus_mutex the_tracer_track_mutex;
janus_mutex the_tracer_race_mutex;
janus_mutex tracer_car_list_mutex;
janus_mutex tracer_driver_list_mutex;

//Tracer Method Definitions

static void tracer_cut_all_controls(char *race_id);
static void tracer_stop_all_streams(char *race_id);
tracer_car *tracer_find_car(char *carid);
tracer_driver *tracer_find_driver(char *driverid);
void tracer_remove_driver(char *driverid);
tracer_driver *tracer_find_driver_by_handle_id(guint64 handle_id);
static void tracer_socket_onData(dyad_Event *e);
static void tracer_socket_onClose(dyad_Event *e);
static void tracer_socket_onAccept(dyad_Event *e);
static void tracer_socket_sendMessage(tracer_car *car, char *message);
void tracer_send_message_to_web_server(json_t *message);
gint granking_compare(gconstpointer ptr_car1, gconstpointer ptr_car2);

//Race Methods
void tracer_on_car_progress_change(tracer_car *car, int progress); //progress is between 0 and 1 for each lap.
void tracer_on_car_pass_finish_line(tracer_car *car, gboolean is_forward);
void tracer_on_car_finish_race(tracer_car *car);
void tracer_on_race_finish(tracer_race *race);

//Tracer helper methods
static void tracer_cut_all_controls(char *race_id)
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (driver->car)
        {
            driver->is_controlling = FALSE;
        }
        tmp_driver_list = tmp_driver_list->next;
    }
    g_free(tmp_driver_list);
    driver = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
}

static void tracer_stop_all_streams(char *race_id)
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (driver->car)
        {
            //Send stop message to plugin
            json_t *message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("stop"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

            //Send stop message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            body = json_object();
            json_object_set_new(body, "request", json_string("stop"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
        }
        tmp_driver_list = tmp_driver_list->next;
    }
    g_free(tmp_driver_list);
    driver = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
}


tracer_car *tracer_find_car(char *carid)
{
    janus_mutex_lock(&tracer_car_list_mutex);
    //Find the car with the given ID
    tracer_car *car = NULL;
    GList *tmp_car_list = g_list_first(tracer_car_list);
    while (tmp_car_list != NULL)
    {
        car = (tracer_car *)tmp_car_list->data;
JANUS_LOG(LOG_INFO, "Comparing car ids: '%s'  '%s'\n", car->id, carid);
        if (!strcasecmp(car->id, carid))
        {
            break;
        }
        car = NULL;
        tmp_car_list = tmp_car_list->next;
    }
    tmp_car_list = NULL;
    janus_mutex_unlock(&tracer_car_list_mutex);
    return car;
}

void tracer_reset_all_cars()
{
    janus_mutex_lock(&tracer_car_list_mutex);
    tracer_car *car = NULL;
    GList *tmp_car_list = g_list_first(tracer_car_list);
    while (tmp_car_list != NULL)
    {
        car = (tracer_car *)tmp_car_list->data;
        if(car){
            car->lap_times = NULL;
            car->finish_time = NULL;
            car->lap_count = 0;
            car->progress = 0;
        }
        car = NULL;
        tmp_car_list = tmp_car_list->next;
    }
    tmp_car_list = NULL;
    janus_mutex_unlock(&tracer_car_list_mutex);
    return car;
}

tracer_driver *tracer_find_driver(char *driverid)
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    //Find the driver with the given ID
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (!strcasecmp(driver->id, driverid))
        {
            break;
        }
        driver = NULL;
        tmp_driver_list = tmp_driver_list->next;
    }
    tmp_driver_list = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
    return driver;
}

void tracer_remove_driver(char *driverid)
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    //Find the driver with the given ID
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (!strcasecmp(driver->id, driverid))
        {
            //If this driver owns a car, we remove him from car's driver.
            if (driver->car != NULL)
            {
                tracer_car *car = tracer_find_car(driver->car);
                car->driver = NULL;
            }
            tracer_driver_list = g_list_remove(tracer_driver_list, tmp_driver_list->data);
            g_free(driver);
            break;
        }
        tmp_driver_list = tmp_driver_list->next;
    }
    tmp_driver_list = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
}

tracer_driver *tracer_find_driver_by_handle_id(guint64 handle_id)
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    //Find the driver with the given ID
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (driver->handle_id_left == handle_id || driver->handle_id_right == handle_id)
        {
            break;
        }
        driver = NULL;
        tmp_driver_list = tmp_driver_list->next;
    }
    tmp_driver_list = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
    return driver;
}

void tracer_disconnect_driver(tracer_driver *driver)
{
    //Send hangup message to plugin
    json_t *message = json_object();
    json_object_set_new(message, "janus", json_string("detach"));
    json_object_set_new(message, "transaction", json_string(driver->id));
    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
    
    message = json_object();
    json_object_set_new(message, "janus", json_string("detach"));
    json_object_set_new(message, "transaction", json_string(driver->id));
    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
    message = NULL;

    //Remove driver from the list
    tracer_remove_driver(driver->id);
}

void tracer_disconnect_all_drivers()
{
    janus_mutex_lock(&tracer_driver_list_mutex);
    tracer_driver *driver = NULL;
    GList *tmp_driver_list = g_list_first(tracer_driver_list);
    while (tmp_driver_list != NULL)
    {
        driver = (tracer_driver *)tmp_driver_list->data;
        if (driver->car)
        {
            //Send stop message to plugin
            json_t *message = json_object();
            json_object_set_new(message, "janus", json_string("detach"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

            //Send stop message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("detach"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
        }
        //Remove driver from the list.
        tracer_driver_list = g_list_remove(tracer_driver_list, tmp_driver_list->data);
        if(driver->car){
            driver->car->driver = NULL;
        }
        g_free(driver);
        tmp_driver_list = tmp_driver_list->next;
    }
    g_free(tmp_driver_list);
    driver = NULL;
    janus_mutex_unlock(&tracer_driver_list_mutex);
}

//Tracer Socket functions for car connection

static void tracer_socket_onData(dyad_Event *e)
{
    struct tracer_car *car;
    if(e->udata != NULL){
        car = (struct tracer_car *)e->udata;
    }
    char *car_id;

    //Process the command
   char command = e->data[0];
    char directionstr[2] = "";
    char speedstr[2] = "";
   switch (command)
   {
        case '2':
                //this is camera servo control command. We should immediately send it back to driver.
                if(car->driver){
                    //Send message to driver throught gateway
                    json_t *request = json_object();
                    json_object_set_new(request, "janus", json_string("message"));
                    json_object_set_new(request, "transaction", json_string("webtrc_message"));
                    json_t *body = json_object();
                    json_object_set_new(body, "request", json_string("sendmessagetodriver"));
                    json_object_set_new(body, "webrtc_message", json_string(e->data));
                    json_object_set_new(request, "body", body);
                    json_object_set_new(request, "session_id", json_integer(tracer_session_id));
                    json_object_set_new(request, "handle_id", json_integer(car->driver->handle_id_left));

                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, 0, request, NULL);
                }
        break;
        default:
            car_id = g_malloc0(e->size);
            memcpy(car_id, e->data, e->size);
            JANUS_LOG(LOG_INFO, "Got a car ID: '%s'\n", car_id);

            car = tracer_find_car(car_id);
            if(car == NULL){
                JANUS_LOG(LOG_INFO, "There is no car in our list with ID: %s. ignoring.\n", car_id);
                return;
            }
            car->is_online = TRUE;
            car->socket_stream = e->stream;

            dyad_removeListener(e->stream, DYAD_EVENT_DATA, tracer_socket_onData, NULL);
            dyad_addListener(e->stream, DYAD_EVENT_DATA, tracer_socket_onData, car);
            // dyad_addListener(e->stream, DYAD_EVENT_CLOSE, tracer_socket_onClose, car);

            //Notify Web Server
            json_t *info = json_object();
            json_object_set_new(info, "info", json_string("carconnected"));
            json_object_set_new(info, "carid", json_string(car_id));
            tracer_send_message_to_web_server(info);
        break;
   }

}

static void tracer_socket_onClose(dyad_Event *e)
{
    //Remove this car from the car list.
    //Car is in e.udata

    struct tracer_car *car = (struct tracer_car *)e->udata;
    car->is_online = FALSE;
    car->socket_stream = NULL;
    JANUS_LOG(LOG_INFO, "Car disconnected ID: %s\n", car->id);

    //Notify Web Server
    json_t *info = json_object();
    json_object_set_new(info, "info", json_string("cardisconnected"));
    json_object_set_new(info, "carid", json_string(car->id));
    tracer_send_message_to_web_server(info);
    car = NULL;
}

static void tracer_socket_onAccept(dyad_Event *e)
{
    dyad_setNoDelay(e->remote, 1);
    dyad_addListener(e->remote, DYAD_EVENT_DATA, tracer_socket_onData, NULL);
    JANUS_LOG(LOG_INFO, "New car connection accepted\n");
}

static void tracer_socket_sendMessage(tracer_car *car, char *message)
{
    if(dyad_getState(car->socket_stream) == DYAD_STATE_CONNECTED){
        if(car->is_online == FALSE){
            car->is_online = TRUE;
            //Notify Web Server
            json_t *info = json_object();
            json_object_set_new(info, "info", json_string("carconnected"));
            json_object_set_new(info, "carid", json_string(car->id));
            tracer_send_message_to_web_server(info);
        }
    }else{
        if(car->is_online == TRUE){
            JANUS_LOG(LOG_INFO, "Car is OFFLINE with id: %s.\n", car->id);
            car->is_online = FALSE;
            //Notify Web Server
            json_t *info = json_object();
            json_object_set_new(info, "info", json_string("cardisconnected"));
            json_object_set_new(info, "carid", json_string(car->id));
            tracer_send_message_to_web_server(info);
            return;
        }
    }
    dyad_writef(car->socket_stream, message);
    JANUS_LOG(LOG_INFO, "Sending message to car -> %s\n", message);
}

void *tracer_race_thread(void *data)
{
    JANUS_LOG(LOG_INFO, "Race thread started\n");

    while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping))
    {
        //We increase elapse time of each race which is started by one.
        janus_mutex_lock(&the_tracer_race_mutex);
    
        if (the_tracer_race && the_tracer_race->is_started && !the_tracer_race->is_paused && !the_tracer_race->is_finished)
        {
            the_tracer_race->elapsed_time++;
            if (the_tracer_race->elapsed_time >= the_tracer_race->max_duration)
                tracer_on_race_finish(the_tracer_race);

            //We send progress updates of driver in the race to drivers and Web Server here
            //update: [{driver_id: "", lap_count: 1, progress: 53}]
            guint64 *driver_handle_ids[4];
            int driver_handle_id_index = 0;
            json_t *updates = json_array();
            janus_mutex_lock(&the_tracer_race->mutex);
            tracer_car *car = NULL;
            GList *tmp_car_list = g_list_first(the_tracer_race->car_list);
            while (tmp_car_list != NULL)
            {
                car = (tracer_car *)tmp_car_list->data;
                if (car && car->driver)
                {
                    driver_handle_ids[driver_handle_id_index++] = car->driver->handle_id_left;

                    json_t *update = json_object();
                    json_object_set_new(update, "driver_id", json_string(car->driver->id));
                    json_object_set_new(update, "lap_count", json_integer(car->lap_count));
                    json_object_set_new(update, "progress", json_integer(car->progress * 100)); //Progress is between 0-1. we make it between 0-100 to make it shorther.
                    json_array_append(updates, update);
                }
                tmp_car_list = tmp_car_list->next;
            }
            tmp_car_list = NULL;
            janus_mutex_unlock(&the_tracer_race->mutex);

            char *payload = json_dumps(updates, json_format);
            for (int d = driver_handle_id_index - 1; d >= 0; d--)
            {
                //Send update message to driver throught gateway
                json_t *request = json_object();
                json_object_set_new(request, "janus", json_string("message"));
                json_object_set_new(request, "transaction", json_string("webtrc_message"));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("sendmessagetodriver"));
                json_object_set_new(body, "webrtc_message", json_string(payload));
                json_object_set_new(request, "body", body);
                json_object_set_new(request, "session_id", json_integer(tracer_session_id));
                json_object_set_new(request, "handle_id", json_integer(driver_handle_ids[d]));

                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, 0, request, NULL);
            }

            json_t *info = json_object();
            json_object_set_new(info, "info", json_string("raceupdate"));
            json_object_set_new(info, "updates", updates);
            tracer_send_message_to_web_server(info);
        }
            
        janus_mutex_unlock(&the_tracer_race_mutex);

        g_usleep(G_USEC_PER_SEC); //wait one second
    }

    JANUS_LOG(LOG_INFO, "Race thread ended\n");
    return NULL;
}
void *tracer_socket_server_thread(void *data)
{
    JANUS_LOG(LOG_INFO, "Socket Server thread started\n");

    dyad_init();
    dyad_setTickInterval(10);

    dyad_Stream *serv = dyad_newStream();
    dyad_addListener(serv, DYAD_EVENT_ACCEPT, tracer_socket_onAccept, NULL);
    //TODO: Get port from config file or web server
    dyad_listen(serv, 8000);
    JANUS_LOG(LOG_INFO, "Socket Server listening at 8000\n");

    while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping))
    {
        dyad_update();
        // JANUS_LOG(LOG_INFO, "dyad_update\n");
    }

    dyad_shutdown();

    JANUS_LOG(LOG_INFO, "Socket Server thread ended\n");
    return NULL;
}

void tracer_send_message_to_web_server(json_t *message)
{
    if (!tracer_webserver_ws_client || !tracer_webserver_ws_client->messages)
        return;

    /* Convert to string and enqueue */
    char *payload = json_dumps(message, json_format);
    g_async_queue_push(tracer_webserver_ws_client->messages, payload);
    lws_callback_on_writable(tracer_webserver_ws_client->wsi);
    janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
    json_decref(message);
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

#ifndef LWS_WITH_IPV6
	JANUS_LOG(LOG_WARN, "libwebsockets has been built without IPv6 support, will bind to IPv4 only\n");
#endif

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
		/* Force single-thread server */
		wscinfo.count_threads = 1;

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
			info.ssl_private_key_password = NULL;
			info.gid = -1;
			info.uid = -1;
			info.options = 0;
			/* Create the WebSocket context */
			wss = lws_create_vhost(wsc, &info);
			if(wss == NULL) {
				JANUS_LOG(LOG_FATAL, "Error creating vhost for WebSockets server...\n");
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
				char *password = NULL;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_key");
				if(item && item->value)
					server_key = (char *)item->value;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_pwd");
				if(item && item->value)
					password = (char *)item->value;
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
				info.ssl_private_key_password = password;
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
					JANUS_LOG(LOG_FATAL, "Error creating vhost for Secure WebSockets server...\n");
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
			info.ssl_private_key_password = NULL;
			info.gid = -1;
			info.uid = -1;
			info.options = 0;
			/* Create the WebSocket context */
			admin_wss = lws_create_vhost(wsc, &info);
			if(admin_wss == NULL) {
				JANUS_LOG(LOG_FATAL, "Error creating vhost for Admin WebSockets server...\n");
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
				char *password = NULL;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_key");
				if(item && item->value)
					server_key = (char *)item->value;
				item = janus_config_get_item_drilldown(config, "certificates", "cert_pwd");
				if(item && item->value)
					password = (char *)item->value;
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
				info.ssl_private_key_password = password;
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
					JANUS_LOG(LOG_FATAL, "Error creating vhost for Secure Admin WebSockets server...\n");
				} else {
					JANUS_LOG(LOG_INFO, "Secure Admin WebSockets server started (port %d)...\n", wsport);
				}
				g_free(ip);
			}
		}
	}
	janus_config_destroy(config);
	config = NULL;



    /* Read Car Mountpoint configuration */
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_STREAMING_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	config = janus_config_parse(filename);
	if(config != NULL)
		janus_config_print(config);

	/* Parse configuration to populate the mountpoints */
	if(config != NULL) {
		/* Iterate on all car mountpoints */
		GList *cl = janus_config_get_categories(config);
		while(cl != NULL) {
			janus_config_category *cat = (janus_config_category *)cl->data;
			if(cat->name == NULL || !strcasecmp(cat->name, "general")) {
				cl = cl->next;
				continue;
			}
			janus_config_item *type = janus_config_get_item(cat, "type");
			if(type == NULL || type->value == NULL) {
				JANUS_LOG(LOG_WARN, "  -- Invalid type, skipping stream '%s'...\n", cat->name);
				cl = cl->next;
				continue;
			}
			if(!strcasecmp(type->value, "rtp")) {
                janus_config_item *mountpoint_id = janus_config_get_item(cat, "id");
                janus_config_item *car_id = janus_config_get_item(cat, "car_id");
                janus_config_item *eye = janus_config_get_item(cat, "eye");
                tracer_car *car = tracer_find_car((char *)car_id->value);
                if(car == NULL){
                    car = g_malloc0(sizeof(tracer_car));
                    car->id = g_malloc0(strlen(car_id->value)+1);
                    memcpy(car->id, car_id->value, strlen(car_id->value));
                    janus_mutex_init(&car->mutex);
                    janus_mutex_lock(&tracer_car_list_mutex);
                    tracer_car_list = g_list_append(tracer_car_list, car);
                    janus_mutex_unlock(&tracer_car_list_mutex);
			        JANUS_LOG(LOG_VERB, "Creating Car with ID: '%s'\n", car->id);
                }
                if(!strcasecmp(eye->value, "left"))
                    car->mountpoint_id_left = g_ascii_strtoull(mountpoint_id->value, 0, 10);
                else
                    car->mountpoint_id_right = g_ascii_strtoull(mountpoint_id->value, 0, 10);
            }
			cl = cl->next;
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
	}
    /* Start the Socket Server service thread */
    socket_server_thread = g_thread_try_new("Socket Server thread", &tracer_socket_server_thread, NULL, &error);
    if (!socket_server_thread)
    {
        g_atomic_int_set(&initialized, 0);
        JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Socket Server thread...\n", error->code, error->message ? error->message : "??");
        return -1;
    }

    /* Start the Socket Server service thread */
    race_thread = g_thread_try_new("Race thread", &tracer_race_thread, NULL, &error);
    if (!race_thread)
    {
        g_atomic_int_set(&initialized, 0);
        JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Race thread...\n", error->code, error->message ? error->message : "??");
        return -1;
    }

    //Create a session
    tracer_webserver_ws_client = g_malloc0(sizeof(janus_websockets_client));
    tracer_webserver_ws_client->destroy = 0;
    janus_mutex_init(&tracer_webserver_ws_client->mutex);

    //Send connect message to plugin
    json_t *request = json_object();
    json_object_set_new(request, "janus", json_string("create"));
    json_object_set_new(request, "transaction", json_string("sessionid"));

    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, 0, request, NULL);


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
    if (socket_server_thread != NULL)
    {
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
	//TODO: Do not notify the core. If we do so, Core will close all webrtc connections. We need to pretend like client always connected.

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
	JANUS_LOG(LOG_INFO, "janus_websockets_send_message -> \n %s \n", json_dumps(message, json_format));
    if(message == NULL)
		return -1;
	// if(transport == NULL) {
	// 	json_decref(message);
	// 	return -1;
	// }
	// /* Make sure this is not related to a closed /freed WebSocket session */
	// janus_mutex_lock(&old_wss_mutex);
	// janus_websockets_client *client = (janus_websockets_client *)transport;
	// if(g_list_find(old_wss, client) != NULL || !client->wsi) {
	// 	json_decref(message);
	// 	message = NULL;
	// 	transport = NULL;
    // 	return -1;
    // }
	// janus_mutex_unlock(&old_wss_mutex);

    //janus_mutex_lock(&tracer_webserver_ws_client->mutex);

    tracer_driver *driver = NULL;
    guint64 handle_id = 0;
    json_t *sender = json_object_get(message, "sender");
    if (sender)
    {
        handle_id = json_integer_value(sender);
        driver = tracer_find_driver_by_handle_id(handle_id);
    }

    const char *janus_text = NULL;
    json_t *j = json_object_get(message, "janus");
    if (!j)
    {
        JANUS_LOG(LOG_INFO, "Janus is NULL. Returning.\n");
        json_decref(message);
        janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
        return -1;
    }
    janus_text = json_string_value(j);
    JANUS_LOG(LOG_INFO, "janus_text : %s\n", janus_text);

    json_t *info = NULL;
    if (!strcasecmp(janus_text, "event"))
    {
        //if plugindata -> data -> result -> status == preparing then send jsep -> sdp to web server as offer

        json_t *plugindata = json_object_get(message, "plugindata");
        if (!plugindata)
        {
            json_decref(message);
            janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
            return 0;
        }
        json_t *data = json_object_get(plugindata, "data");
        if (!data)
        {
            json_decref(message);
            janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
            return 0;
        }
        json_t *streaming = json_object_get(data, "streaming");
        if (!streaming)
        {
            json_decref(message);
            janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
            return 0;
        }
        const char *streaming_text = json_string_value(streaming);
        if (!strcasecmp(streaming_text, "message"))
        {
            //We got message from data channel
            json_t *channelMessage = json_object_get(data, "message");
            if (!channelMessage)
            {
                janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
                return 0;
            }
            //This is the pure message got from data channel. It may be ID of the driver or control command.
            const char *channelMessage_text = json_string_value(channelMessage);
            if (channelMessage_text[0] == '1')
            {
                //This is control command. Send it to car driver controls.
                if (driver->car && driver->is_controlling)
                {
                    char *carCommand = (char *)channelMessage_text;

        JANUS_LOG(LOG_INFO, "Sending commad to car -> %s\n", carCommand);
                    tracer_socket_sendMessage(driver->car, channelMessage_text);
                }
            } else if (channelMessage_text[0] == '2')
            {
                //This is camera control command.
                if (driver->car)
                {
                    char *carCommand = (char *)channelMessage_text;
                    // carCommand[3] = '\0';
                    tracer_socket_sendMessage(driver->car, channelMessage_text);
                }
            }
        }
        else if (!strcasecmp(streaming_text, "event"))
        {
            //We are only interested in offer the plugin sends
            json_t *jsep = json_object_get(message, "jsep");
            if (!jsep)
            {
                janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
                return 0;
            }
            json_t *sdp = json_object_get(jsep, "sdp");

            info = json_object();
            json_object_set_new(info, "info", json_string("offer"));
            json_object_set_new(info, "driverid", json_string(driver->id));
            json_object_set_new(info, "sdp", sdp);
            //Send also if this is offer for left or right.
            if(handle_id == driver->handle_id_left){
                json_object_set_new(info, "isleft", json_true());
            }else{
                json_object_set_new(info, "isleft", json_false());
            }

            goto send_info;
        }
    }
    else if (!strcasecmp(janus_text, "success"))
    {
        json_t *data = json_object_get(message, "data");
        json_t *id = json_object_get(data, "id");
        if (id == NULL)
        {
            JANUS_LOG(LOG_INFO, "Nothing interesting\n");
            janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
            //Nothing interesting
            return 0;
        }

        json_t *sid = json_object_get(message, "session_id");
        if (sid) //If there is session_id, then it is most likely the handle_id came.
        {
            //We come here after an "attach" request. Now we want plugin to send an offer.
            guint64 handle_id = 0;
            handle_id = json_integer_value(id);
            json_t *t = json_object_get(message, "transaction");
            const char *transaction_text = json_string_value(t);
            driver = tracer_find_driver((char *)transaction_text); //We gave driver id as transaction when attaching to the plugin.
            if (driver == NULL)
            {
                janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
                return -1;
            }
            int mountpoint_id = 1;
            //If left handle id is not given yet, we got the left one. We will ask for another attachment for right handle id.
            if(!(driver->handle_id_left > 0)){
                mountpoint_id = 1;
                driver->handle_id_left = handle_id;
                JANUS_LOG(LOG_INFO, "Driver '%s' got LEFT handle id : %" SCNu64 "\n", driver->id, handle_id);
                //Send another attach request.
                json_t *request = json_object();
                json_object_set_new(request, "janus", json_string("attach"));
                json_object_set_new(request, "plugin", json_string("janus.plugin.streaming"));
                json_object_set_new(request, "transaction", json_string(driver->id));
                json_object_set_new(request, "force-bundle", json_true());
                json_object_set_new(request, "force-rtcp-mux", json_true());
                json_object_set_new(request, "session_id", json_integer(tracer_session_id));
                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, request, NULL);
                request = NULL;
            }else{
                mountpoint_id = 2;
                //We got the right and last handle id.
                driver->handle_id_right = handle_id;
                JANUS_LOG(LOG_INFO, "Driver '%s' got RIGHT handle id : %" SCNu64 "\n", driver->id, handle_id);
            }

            json_decref(message);

            //Send connect message to plugin
            json_t *request = json_object();
            json_object_set_new(request, "janus", json_string("message"));
            json_object_set_new(request, "transaction", json_string(driver->id));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("watch"));
            json_object_set_new(body, "id", json_integer(mountpoint_id)); //Just to get some info about mountpoint to create SDP.
            json_object_set_new(request, "body", body);
            json_object_set_new(request, "session_id", json_integer(tracer_session_id));
            json_object_set_new(request, "handle_id", json_integer(handle_id));

            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, 0, request, NULL);
            janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
            return 0;
        }
        janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
        return 0;
    }
    else if (!strcasecmp(janus_text, "webrtcup"))
    {
        if (driver)
            driver->is_online = TRUE;
        info = json_object();
        json_object_set_new(info, "info", json_string("webrtcup"));
        json_object_set_new(info, "driverid", json_string(driver->id));

        goto send_info;
    }
    else if (!strcasecmp(janus_text, "hangup"))
    {
        if (driver)
            driver->is_online = FALSE;
        // info = json_object();
        // json_object_set_new(info, "info", json_string("hangup"));
        // json_object_set_new(info, "driverid", json_string(driver->id));

        goto send_info;
	}

send_info:
{
    //Send info to web server

    if (info == NULL)
        return 0;

    tracer_send_message_to_web_server(info);
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

void janus_websockets_session_created(void *transport, guint64 session_id)
{
    /* We don't care */
    JANUS_LOG(LOG_INFO, "We got our session ID : %" SCNu64 "\n", session_id);
    tracer_session_id = session_id;
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
		case LWS_CALLBACK_GET_THREAD_ID:
			return (uint64_t)pthread_self();
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

/* Race Methods */

//progress is between 0 and 1 for each lap.
void tracer_on_car_progress_change(tracer_car *car, int progress)
{
    if (car->progress > 0.7 && progress < 0.3)
    { //if car was almost end of the track and now at around beginning then he passed the finish line in forward.
        car->progress = progress;
        tracer_on_car_pass_finish_line(car, TRUE);
    }
    else if (car->progress < 0.3 && progress > 0.7)
    { //Drivers went backwards and passed the finish line.
        car->progress = progress;
        tracer_on_car_pass_finish_line(car, FALSE);
    }
    else
    {
        car->progress = progress;
    }
}

void tracer_on_car_pass_finish_line(tracer_car *car, gboolean is_forward)
{
    //Increase lap_count of the car. If car went backward, we remove 1 lap.
    car->lap_count += is_forward ? 1 : -1;
    if (car->lap_count >= the_tracer_race->lap_count)
    {
        tracer_on_car_finish_race(car);
        car->driver->is_controlling = FALSE; //We are taking driver's control over the car, so he wont be able to sabotage others.
        //Send car a stop throttle message.
        char command[6] = {'1', '5', '0', '5', '0', '\0'};
        tracer_socket_sendMessage(car, command);
        return;
    }
    //TODO: Let both driver and Web Server know about lap_count change.
}
void tracer_on_car_finish_race(tracer_car *car)
{
    the_tracer_race->finished_car_count++;
    car->finish_time = the_tracer_race->elapsed_time;
    car->progress = 0;
    the_tracer_race->car_ranking_list[the_tracer_race->finished_car_count - 1] = car;
    if (the_tracer_race->finished_car_count >= the_tracer_race->car_count)
    {
        tracer_on_race_finish(the_tracer_race);
    }
    else
    {
        //TODO: Let both drivers and Web Server know about the driver finishes race.
    }
}
void tracer_on_race_finish(tracer_race *race)
{
    if (race->is_finished) //Already finished.
        return;

    race->is_finished = TRUE;

    //Get the ranking info and send it to both drivers and Web Server.
    race->car_list = g_list_sort(race->car_list, granking_compare);

    //We send ranking of the race to drivers and Web Server here
    //ranking: [{driver_id: "", finished_time: 3333}]
    guint64 *driver_handle_ids[4];
    int driver_handle_id_index = 0;
    json_t *ranking = json_array();
    janus_mutex_lock(&race->mutex);
    tracer_car *car = NULL;
    GList *tmp_car_list = g_list_first(race->car_list);
    while (tmp_car_list != NULL)
    {
        car = (tracer_car *)tmp_car_list->data;
        if (car && car->driver)
        {
            driver_handle_ids[driver_handle_id_index++] = car->driver->handle_id_left;

            json_t *ranking_item = json_object();
            json_object_set_new(ranking_item, "driver_id", json_string(car->driver->id));
            json_object_set_new(ranking_item, "finished_time", json_integer(car->lap_count));
            json_array_append(ranking, ranking_item);
        }
        tmp_car_list = tmp_car_list->next;
    }
    tmp_car_list = NULL;
    janus_mutex_unlock(&race->mutex);

    char *payload = json_dumps(ranking, json_format);
    for (int d = driver_handle_id_index - 1; d >= 0; d--)
    {
        //Send update message to driver throught gateway
        json_t *request = json_object();
        json_object_set_new(request, "janus", json_string("message"));
        json_object_set_new(request, "transaction", json_string("webtrc_message"));
        json_t *body = json_object();
        json_object_set_new(body, "request", json_string("sendmessagetodriver"));
        json_object_set_new(body, "webrtc_message", json_string(payload));
        json_object_set_new(request, "body", body);
        json_object_set_new(request, "session_id", json_integer(tracer_session_id));
        json_object_set_new(request, "handle_id", json_integer(driver_handle_ids[d]));

        gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, request, NULL);
    }

    json_t *info = json_object();
    json_object_set_new(info, "info", json_string("ranking"));
    json_object_set_new(info, "race_id", json_string(race->id));
    json_object_set_new(info, "ranking", ranking);
    tracer_send_message_to_web_server(info);

    tracer_disconnect_all_drivers();
}

void tracer_abort_race(tracer_race *race)
{
    int driver_handle_id_index = 0;
    json_t *abort_message = json_object();
    json_object_set_new(abort_message, "abort", json_integer(0));
    char *payload = json_dumps(abort_message, json_format);
    janus_mutex_lock(&race->mutex);
    tracer_car *car = NULL;
    GList *tmp_car_list = g_list_first(race->car_list);
    while (tmp_car_list != NULL)
    {
        car = (tracer_car *)tmp_car_list->data;
        if (car && car->driver)
        {
            //Send abort message to driver throught gateway
            json_t *request = json_object();
            json_object_set_new(request, "janus", json_string("message"));
            json_object_set_new(request, "transaction", json_string("webtrc_message"));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("sendmessagetodriver"));
            json_object_set_new(body, "webrtc_message", json_string(payload));
            json_object_set_new(request, "body", body);
            json_object_set_new(request, "session_id", json_integer(tracer_session_id));
            json_object_set_new(request, "handle_id", json_integer(car->driver->handle_id_left));
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, request, NULL);

            //TODO: We may need to put a sleep here since we close Webrtc connection just after sending a message.

            //We close the webrtc connection after sending ranking
            request = json_object();
            json_object_set_new(request, "janus", json_string("hangup"));
            json_object_set_new(request, "transaction", json_string("race_aborted"));
            json_object_set_new(request, "session_id", json_integer(tracer_session_id));
            json_object_set_new(request, "handle_id", json_integer(car->driver->handle_id_left));
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, request, NULL);

            request = json_object();
            json_object_set_new(request, "janus", json_string("hangup"));
            json_object_set_new(request, "transaction", json_string("race_aborted"));
            json_object_set_new(request, "session_id", json_integer(tracer_session_id));
            json_object_set_new(request, "handle_id", json_integer(car->driver->handle_id_right));
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, request, NULL);
        }
        tmp_car_list = tmp_car_list->next;
    }
    tmp_car_list = NULL;
    janus_mutex_unlock(&race->mutex);

    tracer_disconnect_all_drivers();
}

gint granking_compare(gconstpointer ptr_car1, gconstpointer ptr_car2)
{
    struct tracer_car *car1;
    struct tracer_car *car2;
    car1 = (tracer_car *)ptr_car1;
    car2 = (tracer_car *)ptr_car2;

    double car1_total_progress = car1->lap_count + car1->progress;
    double car2_total_progress = car2->lap_count + car2->progress;

    if (car1_total_progress > car2_total_progress)
        return 1;
    if (car1_total_progress < car2_total_progress)
        return -1;

    //If their total progresses are same, then they both finished the race before timeout. SO we check their finished_times.
    if (car1->finish_time < car2->finish_time)
        return 1;
    if (car1->finish_time > car2->finish_time)
        return -1;

    return 0;
}

double sqr(double x)
{
    return x * x;
}

#define min(a, b) \
    ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
   _a < _b ? _a : _b; })

#define max(a, b) \
    ({ __typeof__ (a) _a = (a); \
   __typeof__ (b) _b = (b); \
_a > _b ? _a : _b; })

double dist2(double v[], double w[])
{
    return sqr(v[0] - w[0]) + sqr(v[1] - w[1]);
}
double *closestPointOnLineSegment(double p[], double v[], double w[])
{
    double l2 = dist2(v, w);
    if (l2 == 0)
        return p;
    double t = ((p[0] - v[0]) * (w[0] - v[0]) + (p[1] - v[1]) * (w[1] - v[1])) / l2;
    t = max(0, min(1, t));
    double *result = malloc(sizeof(double) * 2);
    result[0] = v[0] + t * (w[0] - v[0]);
    result[1] = v[1] + t * (w[1] - v[1]);
    return result;
}

//Return a value between 0-1. 0 and 1 is the finish line.
double tracer_get_car_track_progress(tracer_track *track, double car_position[2])
{
    for (int l = 1; l < track->line1_points_size; l++)
    {
        track->line1_points[l][2] = track->line1_points[l - 1][2] + sqrt(sqr(track->line1_points[l - 1][0] - track->line1_points[l][0]) + sqr(track->line1_points[l - 1][1] - track->line1_points[l][1]));
    }
    for (int l = 1; l < track->line2_points_size; l++)
    {
        track->line2_points[l][2] = track->line2_points[l - 1][2] + sqrt(sqr(track->line2_points[l - 1][0] - track->line2_points[l][0]) + sqr(track->line2_points[l - 1][1] - track->line2_points[l][1]));
    }

    //find closest segment to the point
    double closestDistance = 999999999;
    double progress;
    for (int l = 1; l < track->line1_points_size; l++)
    {
        double *currentClosestPoint = closestPointOnLineSegment(car_position, track->line1_points[l - 1], track->line1_points[l]);
        double distance = sqrt(dist2(car_position, currentClosestPoint));
        if (distance < closestDistance)
        {
            closestDistance = distance;
            progress = (track->line1_points[l - 1][2] + sqrt(dist2(track->line1_points[l - 1], currentClosestPoint))) / track->line1_points[track->line1_points_size - 1][2];
        }
    }
    for (int l = 1; l < track->line2_points_size; l++)
    {
        double *currentClosestPoint = closestPointOnLineSegment(car_position, track->line2_points[l - 1], track->line2_points[l]);
        double distance = sqrt(dist2(car_position, currentClosestPoint));
        if (distance < closestDistance)
        {
            closestDistance = distance;
            progress = (track->line2_points[l - 1][2] + sqrt(dist2(track->line2_points[l - 1], currentClosestPoint))) / track->line2_points[track->line2_points_size - 1][2];
        }
    }
    return progress;
}

/* This callback handles Janus API requests */
static int janus_websockets_common_callback(
		struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user, void *in, size_t len, gboolean admin)
{
	const char *log_prefix = admin ? "AdminWSS" : "WSS";
	janus_websockets_client *ws_client = (janus_websockets_client *)user;
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

            //TODO: Her baglanti geldiginde atama yapmak yerine, mesaj geldiginde dogru sifreyle gelirse atama yap.
            tracer_webserver_ws_client = ws_client;


            if (the_tracer_track == NULL)
            {
                json_t *info = json_object();
                json_object_set_new(info, "info", json_string("firstconnection"));
                tracer_send_message_to_web_server(info);
            }

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
			if (ws_client == NULL || ws_client->wsi == NULL)
			{
				JANUS_LOG(LOG_ERR, "[%s-%p] Invalid WebSocket client instance...\n", log_prefix, wsi);
				return -1;
			}
			/* Is this a new message, or part of a fragmented one? */
			const size_t remaining = lws_remaining_packet_payload(wsi);
			if (ws_client->incoming == NULL)
			{
				JANUS_LOG(LOG_HUGE, "[%s-%p] First fragment: %zu bytes, %zu remaining\n", log_prefix, wsi, len, remaining);
				ws_client->incoming = g_malloc(len + 1);
				memcpy(ws_client->incoming, in, len);
				ws_client->incoming[len] = '\0';
				JANUS_LOG(LOG_HUGE, "%s\n", ws_client->incoming);
			}
			else
			{
				size_t offset = strlen(ws_client->incoming);
				JANUS_LOG(LOG_HUGE, "[%s-%p] Appending fragment: offset %zu, %zu bytes, %zu remaining\n", log_prefix, wsi, offset, len, remaining);
				ws_client->incoming = g_realloc(ws_client->incoming, offset+len+1);
				memcpy(ws_client->incoming+offset, in, len);
				ws_client->incoming[offset+len] = '\0';
				JANUS_LOG(LOG_HUGE, "%s\n", ws_client->incoming+offset);
			}
			if(remaining > 0) // || !lws_is_final_fragment(wsi))
			{
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
            return 0;
        command_text = json_string_value(command);
        JANUS_LOG(LOG_INFO, "command_text : %s\n", command_text);

        //Find the car with the given ID
        tracer_car *car = NULL;
        tracer_driver *driver = NULL;
        const char *carId = NULL;
        const char *driverId = NULL;
        
       
        json_t *carid = json_object_get(root, "carid");
        if (carid)
        {
            carId = json_string_value(carid);
            car = tracer_find_car(carId);
            if (car == NULL)
            {
                //TODO: Handle this. Let web server know that there is no car with this id.
                JANUS_LOG(LOG_INFO, "There is no car with ID : %s\n", carId);

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
                //TODO: Handle this. Let web server know that there is no driver with this id.
                JANUS_LOG(LOG_ERR, "There is no driver with ID : %s\n", driverId);

                return 0;
            }
        }
        json_t *message = NULL;
        if (!strcasecmp(command_text, "startsession"))
        {
            //We are not using this right now. We create a session in init. Timeout is set to 0, disabled so we do not need to create a new session unless we disconnect from Web Server.
            //Send connect message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("create"));
            json_object_set_new(message, "transaction", json_string("sessionid"));

            goto send_message;
        }
        else if (!strcasecmp(command_text, "settracklines"))
        {
            //This message includes track line coordinates.
            if (the_tracer_track == NULL)
            {
                the_tracer_track = g_malloc0(sizeof(tracer_track));
                janus_mutex_init(&the_tracer_track->mutex);
            }
            //If track already exist, we only update its line point lists.
            /* Put line 1 points */
            size_t line1rowindex;
            json_t *list1row;
            json_t *pointlist1 = json_object_get(root, "list1");
            free(the_tracer_track->line1_points); //first free the old list.
            the_tracer_track->line1_points_size = json_array_size(pointlist1);
            JANUS_LOG(LOG_INFO, "size of list1-> %d\n", the_tracer_track->line1_points_size);
            the_tracer_track->line1_points = malloc(the_tracer_track->line1_points_size * sizeof(*the_tracer_track->line1_points));
            JANUS_LOG(LOG_INFO, "\n\n");
            json_array_foreach(pointlist1, line1rowindex, list1row)
            {
                size_t valueindex;
                json_t *list1value;
                json_array_foreach(list1row, valueindex, list1value)
                {
                    the_tracer_track->line1_points[line1rowindex][valueindex] = json_real_value(list1value);
                    JANUS_LOG(LOG_INFO, "%f   ", the_tracer_track->line1_points[line1rowindex][valueindex]);
                }
                JANUS_LOG(LOG_INFO, "\n");
            }
            JANUS_LOG(LOG_INFO, "\n\n");

            /* Put line 2 points */
            size_t line2rowindex;
            json_t *list2row;
            json_t *pointlist2 = json_object_get(root, "list2");
            free(the_tracer_track->line2_points); //first free the old list.
            the_tracer_track->line2_points_size = json_array_size(pointlist2);
            JANUS_LOG(LOG_INFO, "size of list2-> %d\n", the_tracer_track->line2_points_size);
            the_tracer_track->line2_points = malloc(the_tracer_track->line2_points_size * sizeof(*the_tracer_track->line2_points));
            JANUS_LOG(LOG_INFO, "\n\n");
            json_array_foreach(pointlist2, line2rowindex, list2row)
            {
                size_t valueindex;
                json_t *list2value;
                json_array_foreach(list2row, valueindex, list2value)
                {
                    the_tracer_track->line2_points[line2rowindex][valueindex] = json_real_value(list2value);
                    JANUS_LOG(LOG_INFO, "%f   ", the_tracer_track->line2_points[line2rowindex][valueindex]);
                }
                JANUS_LOG(LOG_INFO, "\n");
            }
            JANUS_LOG(LOG_INFO, "\n\n");
        }
        else if (!strcasecmp(command_text, "createrace"))
        {
            json_t *newraceid = json_object_get(root, "id");
            if (!newraceid)
            {
                JANUS_LOG(LOG_ERR, "There is no race id: createrace request.\n");
                return 0;
            }
            if (the_tracer_track == NULL)
            {
                JANUS_LOG(LOG_ERR, "The Track is null: createrace request.\n");
                return 0;
            }

            if(the_tracer_race){
                tracer_disconnect_all_drivers();
                g_free(the_tracer_race);
                the_tracer_race = NULL;
            }

            tracer_reset_all_cars();

            const char *newRaceId = json_string_value(newraceid);
            the_tracer_race = g_malloc0(sizeof(tracer_race));
            the_tracer_race->id = (char *)newRaceId;
            janus_mutex_init(&the_tracer_race->mutex);

            json_t *max_duration = json_object_get(root, "max_duration");
            if (max_duration)
            {
                the_tracer_race->max_duration = json_integer_value(max_duration);
            }
            else
            {
                the_tracer_race->max_duration = 0;
            }

            the_tracer_race->track = the_tracer_track;
            the_tracer_race->is_started = FALSE;
            the_tracer_race->is_paused = FALSE;
            the_tracer_race->is_finished = FALSE;

            size_t driver_id_index;
            json_t *driver_id;
            json_t *driver_ids = json_object_get(root, "driver_ids");
            the_tracer_race->car_count = 0;
            JANUS_LOG(LOG_INFO, "number of driver ids -> %d\n", json_array_size(driver_ids));
            json_array_foreach(driver_ids, driver_id_index, driver_id)
            {
                char *driver_id_string = json_string_value(driver_id);
                tracer_driver *driver = tracer_find_driver(driver_id_string);

                if (driver == NULL)
                {
                    //We create a new driver. And create a handle for him by attach request.
                    driver = g_malloc0(sizeof(tracer_driver));
                    janus_mutex_init(&driver->mutex);
                    driver->id = (char *)driver_id_string;
                    driver->is_online = FALSE;
                    driver->is_controlling = FALSE;
                    janus_mutex_lock(&tracer_driver_list_mutex);
                    tracer_driver_list = g_list_append(tracer_driver_list, driver);
                    janus_mutex_unlock(&tracer_driver_list_mutex);
                    JANUS_LOG(LOG_INFO, "New driver added to the list : %s\n", driver_id_string);
                }
                if (driver->handle_id_left == NULL)
                {
                    message = json_object();
                    json_object_set_new(message, "janus", json_string("attach"));
                    json_object_set_new(message, "plugin", json_string("janus.plugin.streaming"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    json_object_set_new(message, "force-bundle", json_true());
                    json_object_set_new(message, "force-rtcp-mux", json_true());
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, &error);
                    message = NULL;
                }
            }
        }
        else if (!strcasecmp(command_text, "startrace"))
        {
            //TODO: Do a countdown before starting the race.
            janus_mutex_lock(&the_tracer_race->mutex);
            the_tracer_race->start_time = janus_get_monotonic_time();
            the_tracer_race->is_started = TRUE;
            the_tracer_race->is_paused = FALSE;
            the_tracer_race->is_finished = FALSE;
            janus_mutex_unlock(&the_tracer_race->mutex);

            tracer_car *car = NULL;
            GList *tmp_car_list = g_list_first(tracer_car_list);
            while (tmp_car_list != NULL)
            {
                car = (tracer_car *)tmp_car_list->data;
                if (car->driver)
                {
                    car->driver->is_controlling = TRUE;
                }
                tmp_car_list = tmp_car_list->next;
            }
            tmp_car_list = NULL;
        }
        else if (!strcasecmp(command_text, "pauserace"))
        {
            the_tracer_race->is_paused = TRUE;
        }
        else if (!strcasecmp(command_text, "resumerace"))
        {
            the_tracer_race->is_paused = FALSE;
        }
        else if (!strcasecmp(command_text, "endrace"))
        {
            tracer_on_race_finish(the_tracer_race);
        }
        else if (!strcasecmp(command_text, "abortrace"))
        {
            tracer_abort_race(the_tracer_race);
        }
        else if (!strcasecmp(command_text, "connecttodriver"))
        {
            //Call attach to create new handle
            //{"janus":"attach","plugin":"janus.plugin.streaming","transaction":"wv662x7enjVe","force-bundle":true,"force-rtcp-mux":true}
            //Prepare 'connect' command and send it to streaming plugin through gateway.
            //{"janus":"message", "body" : {"request":"watch", "id" : 2}, "transaction" : "EtKVfSvaJYrU"}

            //Check if there is a driver with the given ID. If so, just send a webrtcup message to web server.
            json_t *newdriverid = json_object_get(root, "id");
            if (!newdriverid)
            {
                JANUS_LOG(LOG_ERR, "There is no driver ID in the connecttodriver request.\n");
                return 0;
            }
            const char *newDriverId = json_string_value(newdriverid);

            driver = tracer_find_driver(newDriverId);
            if (driver == NULL){
                driver = g_malloc0(sizeof(tracer_driver));
                janus_mutex_init(&driver->mutex);
                driver->id = (char *)newDriverId;
                driver->car = NULL;
                janus_mutex_lock(&tracer_driver_list_mutex);
                tracer_driver_list = g_list_append(tracer_driver_list, driver);
                janus_mutex_unlock(&tracer_driver_list_mutex);
                JANUS_LOG(LOG_INFO, "New driver added to the list : %s\n", newDriverId);
            }

            if (driver->is_online)
            {
                json_t *info = json_object();
                json_object_set_new(info, "info", json_string("webrtcup"));
                json_object_set_new(info, "driverid", json_string(driver->id));

                janus_mutex_lock(&tracer_webserver_ws_client->mutex);
                janus_mutex_lock(&old_wss_mutex);
                //Send info to web server
                /* Convert to string and enqueue */
                char *payload = json_dumps(info, json_format);
                g_async_queue_push(tracer_webserver_ws_client->messages, payload);
                lws_callback_on_writable(tracer_webserver_ws_client->wsi);
                janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
                janus_mutex_unlock(&old_wss_mutex);
                json_decref(info);
                json_decref(message);
                return 0;
            }
            else
            {
                if (driver->handle_id_right)
                {
                    //We only send a connect message to plugin.
                    message = json_object();
                    json_object_set_new(message, "janus", json_string("message"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    json_t *body = json_object();
                    json_object_set_new(body, "request", json_string("connect"));
                    json_object_set_new(body, "id", json_integer(1)); //Just to get some info about tracer_mountpoint_list to create SDP.
                    json_object_set_new(message, "body", body);
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

                    message = json_object();
                    json_object_set_new(message, "janus", json_string("message"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    body = json_object();
                    json_object_set_new(body, "request", json_string("connect"));
                    json_object_set_new(body, "id", json_integer(1)); //Just to get some info about tracer_mountpoint_list to create SDP.
                    json_object_set_new(message, "body", body);
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
                    message = NULL;
                }
                else
                {
                    message = json_object();
                    json_object_set_new(message, "janus", json_string("attach"));
                    json_object_set_new(message, "plugin", json_string("janus.plugin.streaming"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    json_object_set_new(message, "force-bundle", json_true());
                    json_object_set_new(message, "force-rtcp-mux", json_true());
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));

                }

                goto send_message;
            }
        }
        else if (!strcasecmp(command_text, "disconnectdriver"))
        {
            tracer_disconnect_driver(driver);

            goto send_message;
        }
        else if (!strcasecmp(command_text, "startstream"))
        {
            //#############################################################
            //REMOVE HERE

            //Send startstream message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("startstream"));
            json_object_set_new(body, "id", json_integer(1));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            body = json_object();
            json_object_set_new(body, "request", json_string("startstream"));
            json_object_set_new(body, "id", json_integer(1));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            message = NULL;

            JANUS_LOG(LOG_INFO, "Sending Start Stream command to the Camera\n");

            goto send_message;

            //REMOVE HERE
            //#############################################################



            // if (driver->car == NULL)
            // {
            //     if (car == NULL)
            //     {
            //         //TODO: Tell Web server that this driver has no car yet.
            //         return;
            //     }else if (car->driver != NULL)
            //     {
            //         //If this car has already a driver, we set that driver's car to NULL since this car has a new owner.
            //         car->driver->car = NULL;
            //     }
            //     driver->car = car;
            //     car->driver = driver;
            // }
            // //{janus: "message", body: {request: "startstream", id: 1}, transaction: "d3pNxxpfMd3d"}

            // //Send startstream message to plugin
            // message = json_object();
            // json_object_set_new(message, "janus", json_string("message"));
            // json_object_set_new(message, "transaction", json_string(driver->id));
            // json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            // json_object_set_new(message, "handle_id", json_integer(driver->handle_id));
            // json_t *body = json_object();
            // json_object_set_new(body, "request", json_string("startstream"));
            // json_object_set_new(body, "id", json_integer(driver->car->mountpoint_id_left));
            // json_object_set_new(message, "body", body);

            // JANUS_LOG(LOG_INFO, "Sending Start Stream command to the Camera\n");

            // goto send_message;
        }
        else if (!strcasecmp(command_text, "watch"))
        {
            //Call attach to create new handle
            //{"janus":"attach","plugin":"janus.plugin.streaming","transaction":"wv662x7enjVe","force-bundle":true,"force-rtcp-mux":true}
            //Prepare 'connect' command and send it to streaming plugin through gateway.
            //{"janus":"message", "body" : {"request":"watch", "id" : 2}, "transaction" : "EtKVfSvaJYrU"}

            //Check if there is a driver with the given ID. If so, just send a webrtcup message to web server.
            json_t *newdriverid = json_object_get(root, "id");
            if (!newdriverid)
            {
                JANUS_LOG(LOG_ERR, "There is no driver ID in the watch request.\n");
                return 0;
            }
            const char *newDriverId = json_string_value(newdriverid);

            driver = tracer_find_driver(newDriverId);
            if (driver == NULL){
                driver = g_malloc0(sizeof(tracer_driver));
                janus_mutex_init(&driver->mutex);
                driver->id = (char *)newDriverId;
                driver->car = NULL;
                janus_mutex_lock(&tracer_driver_list_mutex);
                tracer_driver_list = g_list_append(tracer_driver_list, driver);
                janus_mutex_unlock(&tracer_driver_list_mutex);
                JANUS_LOG(LOG_INFO, "New driver added to the list : %s\n", newDriverId);
            }

            if (driver->is_online)
            {
                json_t *info = json_object();
                json_object_set_new(info, "info", json_string("webrtcup"));
                json_object_set_new(info, "driverid", json_string(driver->id));

                janus_mutex_lock(&tracer_webserver_ws_client->mutex);
                janus_mutex_lock(&old_wss_mutex);
                //Send info to web server
                /* Convert to string and enqueue */
                char *payload = json_dumps(info, json_format);
                g_async_queue_push(tracer_webserver_ws_client->messages, payload);
                lws_callback_on_writable(tracer_webserver_ws_client->wsi);
                janus_mutex_unlock(&tracer_webserver_ws_client->mutex);
                janus_mutex_unlock(&old_wss_mutex);
                json_decref(info);
                json_decref(message);
                return 0;
            }
            else
            {
                if (driver->handle_id_right)
                {
                    //We only send a connect message to plugin.
                    message = json_object();
                    json_object_set_new(message, "janus", json_string("message"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    json_t *body = json_object();
                    json_object_set_new(body, "request", json_string("watch"));
                    json_object_set_new(body, "id", json_integer(1)); //Just to get some info about tracer_mountpoint_list to create SDP.
                    json_object_set_new(message, "body", body);
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

                    message = json_object();
                    json_object_set_new(message, "janus", json_string("message"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    body = json_object();
                    json_object_set_new(body, "request", json_string("watch"));
                    json_object_set_new(body, "id", json_integer(1)); //Just to get some info about tracer_mountpoint_list to create SDP.
                    json_object_set_new(message, "body", body);
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                    json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
                    gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
                    message = NULL;
                }
                else
                {
                    message = json_object();
                    json_object_set_new(message, "janus", json_string("attach"));
                    json_object_set_new(message, "plugin", json_string("janus.plugin.streaming"));
                    json_object_set_new(message, "transaction", json_string(driver->id));
                    json_object_set_new(message, "force-bundle", json_true());
                    json_object_set_new(message, "force-rtcp-mux", json_true());
                    json_object_set_new(message, "session_id", json_integer(tracer_session_id));

                }

                goto send_message;
            }

        }
        else if (!strcasecmp(command_text, "startrecording"))
        {
            //#############################################################
            //REMOVE HERE

            //Send startstream message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("recording"));
            json_object_set_new(body, "action", json_string("start"));
            json_object_set_new(body, "video", json_string("/tmp/video.mjr"));
            json_object_set_new(body, "id", json_integer(1));
            json_object_set_new(message, "body", body);

            JANUS_LOG(LOG_INFO, "Starting to recording!\n");

            goto send_message;

            //REMOVE HERE
            //#############################################################


        }
        else if (!strcasecmp(command_text, "stoprecording"))
        {
            //#############################################################
            //REMOVE HERE

            //Send startstream message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("recording"));
            json_object_set_new(body, "action", json_string("stop"));
            json_object_set_new(body, "video", json_true());
            json_object_set_new(body, "id", json_integer(1));
            json_object_set_new(message, "body", body);

            JANUS_LOG(LOG_INFO, "Stopping to recording!\n");

            goto send_message;

            //REMOVE HERE
            //#############################################################


        }
        else if (!strcasecmp(command_text, "startstreamandcontrol"))
        {
            if (driver->car == NULL)
            {
                if (car == NULL)
                {
                    //TODO: Tell Web server that this driver has no car yet.
                }
                if (car->driver != NULL)
                {
                    //If this car has already a driver, we set that driver's car to NULL since this car has a new owner.
                    car->driver->car = NULL;
                }
                driver->car = car;
                car->driver = driver;
            }

            //{janus: "message", body: {request: "startstream", id: 1}, transaction: "d3pNxxpfMd3d"}

            //Send watch message to plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("startstream"));
            json_object_set_new(body, "id", json_integer(driver->car->mountpoint_id_left));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            body = json_object();
            json_object_set_new(body, "request", json_string("startstream"));
            json_object_set_new(body, "id", json_integer(driver->car->mountpoint_id_right));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            message = NULL;

            JANUS_LOG(LOG_INFO, "Sending Start Stream command to the Camera\n");

            //Rely driver commands to the car
            driver->is_controlling = TRUE;

            goto send_message;
        }
        else if (!strcasecmp(command_text, "stopstreamandcontrol"))
        {
            if (driver->car == NULL)
            {
                //TODO: Tell Web server that this driver has no car yet.
            }
            //{janus: "message", body: {request: "stopstream"}, transaction: "d3pNxxpfMd3d"}

            //Send stop message to the plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("stopstream"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            body = json_object();
            json_object_set_new(body, "request", json_string("stopstream"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            message = NULL;

            JANUS_LOG(LOG_INFO, "Sending Stop Stream command to the Camera\n");

            //Do not relay driver commands to the car
            driver->is_controlling = FALSE;

            goto send_message;
        }
        else if (!strcasecmp(command_text, "stopstream"))
        {
            if (driver->car == NULL)
            {
                //TODO: Tell Web server that this driver has no car yet.
                return 0;
            }

            //Send stop message to the plugin
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("stopstream"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
            body = json_object();
            json_object_set_new(body, "request", json_string("stopstream"));
            json_object_set_new(message, "body", body);
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
            message = NULL;

            JANUS_LOG(LOG_INFO, "Sending Pause Stream command to the Camera\n");

            goto send_message;
        }
        else if (!strcasecmp(command_text, "setdriverofcar"))
        {
            if(car == NULL || driver == NULL)
                return 0;

            if (car->driver != NULL)
            {
                //If this car has already a driver, we set that driver's car to NULL since this car has a new owner.
                car->driver->car = NULL;

                //We first stop stream to the current driver of the car.
                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(car->driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(car->driver->handle_id_left));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("stopstream"));
                json_object_set_new(message, "body", body);
                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);

                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(car->driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(car->driver->handle_id_right));
                body = json_object();
                json_object_set_new(body, "request", json_string("stopstream"));
                json_object_set_new(message, "body", body);
                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
                message = NULL;
            }
            if (driver->car != NULL)
            {
                //If this driver has already a car, we set that car's driver to NULL since this driver has a new car.
                driver->car->driver = NULL;

                //Then we stop stream of driver's current car.
                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(car->driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(car->driver->handle_id_left));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("stopstream"));
                json_object_set_new(message, "body", body);
                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
                
                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(car->driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(car->driver->handle_id_right));
                body = json_object();
                json_object_set_new(body, "request", json_string("stopstream"));
                json_object_set_new(message, "body", body);
                gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, NULL);
                message = NULL;
            }
            //Set car's owner
            car->driver = driver;
            driver->car = car;
            driver->is_controlling = TRUE;
            return 0;
        }
        else if (!strcasecmp(command_text, "addcontrol"))
        {
            //Let driver control his car
            driver->is_controlling = TRUE;
            return 0;
        }
        else if (!strcasecmp(command_text, "removecontrol"))
        {
            driver->is_controlling = FALSE;
            return 0;
        }
        else if (!strcasecmp(command_text, "removeallcontrols"))
        {
            //For each in driver list and set their controllingCar to NULL
            tracer_cut_all_controls(the_tracer_race->id);
            return 0;
        }
        else if (!strcasecmp(command_text, "stopallstreams"))
        {
            //For each in driver list and send stop message to plugin with their handle ids
            tracer_stop_all_streams(the_tracer_race->id);
            return 0;
        }
        else if (!strcasecmp(command_text, "controlcar"))
        {
            if(car == NULL)
                return 0;
                   
            //Send speed and steering values to the car
            const char *throttle_str = "50";
            const char *steering_str = "50";
            json_t *throttle = json_object_get(root, "throttle");
            if (throttle && json_is_string(throttle))
                throttle_str = json_string_value(throttle);
            json_t *steering = json_object_get(root, "steering");
            if (steering && json_is_string(steering))
                steering_str = json_string_value(steering);

            char command[6] = {'1', throttle_str[0], throttle_str[1], steering_str[0], steering_str[1], '\0'};

            tracer_socket_sendMessage(car, command);
            return 0;
        }
        else if (!strcasecmp(command_text, "setstreamurl"))
        {
            //TODO: Fix this for two cameras.
            json_t *url = json_object_get(root, "url");
            if (!url)
            {
                JANUS_LOG(LOG_ERR, "There is no url: setstreamurl request.\n");
                return 0;
            }
            char *URL = json_string_value(url);

            //{"janus":"message","body":{"request":"create","type":"rtsp", "audio":1, video:1, permanent:1 id:"1234", name:"1234", url:"rtsp://192.168.1.26"},"transaction":"765EejXJtvRi"}
            message = json_object();
            json_object_set_new(message, "janus", json_string("message"));
            json_object_set_new(message, "transaction", json_string(driver->id));
            json_object_set_new(message, "session_id", json_integer(tracer_session_id));
            json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
            json_t *body = json_object();
            json_object_set_new(body, "request", json_string("create"));
            json_object_set_new(body, "type", json_string("rtsp"));
            json_object_set_new(body, "audio", json_true());
            json_object_set_new(body, "video", json_true());
            json_object_set_new(body, "permanent", json_true());
            json_object_set_new(body, "id", json_integer(carId));
            char carId_str[12];
            sprintf(carId_str, "%d", carId);
            json_object_set_new(body, "name", json_string(carId_str));
            json_object_set_new(body, "url", json_string(URL));
            json_object_set_new(message, "body", body);

            goto send_message;
        }
        else if (!strcasecmp(command_text, "answersdp"))
        {
            //Send answer to Janus Core
            json_t *answersdp = json_object_get(root, "answersdp");
            json_t *isleft = json_object_get(root, "isleft");

            //{"janus":"message","body":{"request":"start"},"transaction":"765EejXJtvRi","jsep":{"type":"answer","sdp":""}}
            if(isleft == json_true()){
                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("acceptanswer")); //Normally, answer comes with a "start" request. Check if it works without any request.
                json_object_set_new(message, "body", body);
                json_object_set_new(message, "jsep", answersdp);
            }else{
                message = json_object();
                json_object_set_new(message, "janus", json_string("message"));
                json_object_set_new(message, "transaction", json_string(driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("acceptanswer")); //Normally, answer comes with a "start" request. Check if it works without any request.
                json_object_set_new(message, "body", body);
                json_object_set_new(message, "jsep", answersdp);
            }

            goto send_message;
        }
        else if (!strcasecmp(command_text, "candidate"))
        {
            //Send answer to Janus Core
            json_t *candidate = json_object_get(root, "candidate");
            json_t *isleft = json_object_get(root, "isleft");

            //{"janus":"message","body":{"request":"start"},"transaction":"765EejXJtvRi","jsep":{"type":"answer","sdp":""}}
            if(isleft == json_true()){
                message = json_object();
                json_object_set_new(message, "janus", json_string("trickle"));
                json_object_set_new(message, "transaction", json_string(driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(driver->handle_id_left));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("acceptanswer")); //Normally, answer comes with a "start" request. It works like this too. acceptanswer is an empty method in streaming plugin.
                json_object_set_new(message, "body", body);
                json_object_set_new(message, "candidate", candidate);
            }else{
                message = json_object();
                json_object_set_new(message, "janus", json_string("trickle"));
                json_object_set_new(message, "transaction", json_string(driver->id));
                json_object_set_new(message, "session_id", json_integer(tracer_session_id));
                json_object_set_new(message, "handle_id", json_integer(driver->handle_id_right));
                json_t *body = json_object();
                json_object_set_new(body, "request", json_string("acceptanswer")); //Normally, answer comes with a "start" request. It works like this too. acceptanswer is an empty method in streaming plugin.
                json_object_set_new(message, "body", body);
                json_object_set_new(message, "candidate", candidate);
            }

            goto send_message;
        }

    send_message:

        if (message != NULL)
        {
            JANUS_LOG(LOG_INFO, "Sending message to Janus Core");
            gateway->incoming_request(&janus_websockets_transport, tracer_webserver_ws_client, NULL, FALSE, message, &error);
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
        //givecontrol: Let driver control the car. Also start streaming.
        //removecontrol: Stop letting driver control the car.
        //removeallcontrols: Stops letting all drivers control their cars.
        //stopallstreams: Send stop stream command to all cars. Stop all controls.
        //startallstreams: Send start stream command to all cars.
        //addallcontrols: Send start stream command to all cars. Start all controls.
        //controlcar: includes carId, speed and steering values.

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
					if (buflen > ws_client->buflen) {
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
