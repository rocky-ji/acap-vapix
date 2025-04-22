#include <curl/curl.h>
#include <gio/gio.h>
#include <jansson.h>
#include <syslog.h>

#define MAX_STR_LENGTH 1024
#define MAX_DATA_LENGTH 1024*20
gchar g_credentials[MAX_STR_LENGTH];
gboolean SendRecv(char* strUrl, char* strUserPass, char* strRecv);
__attribute__((noreturn)) __attribute__((format(printf, 1, 2))) static void
panic(const char* format, ...) {
    va_list arg;
    va_start(arg, format);
    vsyslog(LOG_ERR, format, arg);
    va_end(arg);
    exit(1);
}

static char* parse_credentials(GVariant* result) {
    char* credentials_string = NULL;
    char* id                 = NULL;
    char* password           = NULL;

    g_variant_get(result, "(&s)", &credentials_string);
    if (sscanf(credentials_string, "%m[^:]:%ms", &id, &password) != 2)
        panic("Error parsing credential string '%s'", credentials_string);
    char* credentials = g_strdup_printf("%s:%s", id, password);

    free(id);
    free(password);
    return credentials;
}

static char* retrieve_vapix_credentials(const char* username) {
    GError* error               = NULL;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection)
        panic("Error connecting to D-Bus: %s", error->message);

    const char* bus_name       = "com.axis.HTTPConf1";
    const char* object_path    = "/com/axis/HTTPConf1/VAPIXServiceAccounts1";
    const char* interface_name = "com.axis.HTTPConf1.VAPIXServiceAccounts1";
    const char* method_name    = "GetCredentials";

    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   bus_name,
                                                   object_path,
                                                   interface_name,
                                                   method_name,
                                                   g_variant_new("(s)", username),
                                                   NULL,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   NULL,
                                                   &error);
    if (!result)
        panic("Error invoking D-Bus method: %s", error->message);

    char* credentials = parse_credentials(result);

    g_variant_unref(result);
    g_object_unref(connection);
    return credentials;
}

static size_t append_to_gstring_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t processed_bytes = size * nmemb;
    g_string_append_len((GString*)userdata, ptr, processed_bytes);
    return processed_bytes;
}

static char*
vapix_post(CURL* handle, const char* credentials, const char* endpoint, const char* request) {
    GString* response = g_string_new(NULL);
    char* url         = g_strdup_printf("http://127.0.0.12/axis-cgi/%s", endpoint);

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERPWD, credentials);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_to_gstring_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, response);

    CURLcode res = curl_easy_perform(handle);
    if (res != CURLE_OK)
        panic("curl_easy_perform error %d: '%s'", res, curl_easy_strerror(res));

    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200)
        panic("Got response code %ld from request to %s with response '%s'",
              response_code,
              request,
              response->str);

    free(url);
    return g_string_free(response, FALSE);
}

static json_t*
vapix_post_json(CURL* handle, const char* credentials, const char* endpoint, const char* request) {
    char* text_response = vapix_post(handle, credentials, endpoint, request);
    json_error_t parse_error;
    json_t* json_response = json_loads(text_response, 0, &parse_error);
    if (!json_response)
        panic("Invalid JSON response: %s", parse_error.text);

    const json_t* request_error = json_object_get(json_response, "error");
    if (request_error)
        panic("Failed to perform request: %s",
              json_string_value(json_object_get(request_error, "message")));

    free(text_response);
    return json_response;
}

static json_t* get_all_properties(CURL* handle, const char* credentials) {
    const char* endpoint = "basicdeviceinfo.cgi";
    const char* request =
        "{"
        "  \"apiVersion\": \"1.3\","
        "  \"method\": \"getAllProperties\""
        "}";
    return vapix_post_json(handle, credentials, endpoint, request);
}

static const char* read_property(const json_t* all_props, const char* prop_name) {
    const json_t* data       = json_object_get(all_props, "data");
    const json_t* prop_list  = json_object_get(data, "propertyList");
    const json_t* prop_value = json_object_get(prop_list, prop_name);
    return json_string_value(prop_value);
}

static size_t writer(char *data, size_t size, size_t nmemb, void *writerData)
{
	size_t nbytes = size * nmemb;
    if (writerData == NULL) 
		return 0;    

	strncat((char *)writerData, data, nbytes);
	//g_message("writer:%s\n", (char *)writerData);	

    return nbytes;
}

//"http://192.168.77.179/axis-cgi/param.cgi?action=list&group=Image.I0.Overlay.Enabled"
gboolean SendRecv(char* strUrl, char* strUserPass, char* strRecv)
{
	CURL *curl;
	CURLcode res;
	char strData[MAX_STR_LENGTH];
	memset(strData, 0, MAX_STR_LENGTH);

	curl = curl_easy_init();
	if(curl) {
		res = curl_easy_setopt(curl, CURLOPT_URL, strUrl);

		res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (void *)&writer);
		res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, strRecv);

		res = curl_easy_setopt(curl, CURLOPT_USERPWD, strUserPass);
		res = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);//CURLAUTH_DIGEST|CURLAUTH_BASIC); 

		res = curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);


		res = curl_easy_perform(curl);

		if(res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_perform() failed: %s\n",	curl_easy_strerror(res));
			syslog(LOG_INFO, "curl_easy_perform() failed: %s",curl_easy_strerror(res));
			curl_easy_cleanup(curl);
			return FALSE;
		}


		curl_easy_cleanup(curl);
	}
	return TRUE;
}

int main(void) {
    openlog(NULL, LOG_PID, LOG_USER);

    syslog(LOG_INFO, "Curl version %s", curl_version_info(CURLVERSION_NOW)->version);
    syslog(LOG_INFO, "Jansson version %s", JANSSON_VERSION);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* handle = curl_easy_init();

    char* credentials = retrieve_vapix_credentials("example-vapix-user");
    sprintf(g_credentials,"%s", credentials);
    syslog(LOG_INFO, "credentials£º%s", g_credentials);//don't  do this way in release.
    
    char strUrl[MAX_STR_LENGTH];
	char strData[MAX_DATA_LENGTH];

	memset(strUrl, 0, MAX_STR_LENGTH);
	memset(strData, 0, MAX_DATA_LENGTH);	
  
    //sprintf(strUrl, "http://127.0.0.12/axis-cgi/param.cgi?action=list&group=root.ImageSource.I0.Sensor.Brightness");	
    //sprintf(strUrl, "http://127.0.0.12/axis-cgi/pwdgrp.cgi?action=add&user=zzzzzzzzz&pwd=yyy&grp=users&sgrp=viewer&comment=zzz");	
	sprintf(strUrl, "http://127.0.0.12/axis-cgi/applications/control.cgi?action=start&package=send_event");

	if(FALSE == SendRecv(strUrl, g_credentials, strData))
	{
		//error to overlay
		syslog(LOG_INFO, "failed to start add user------------");
	}
	syslog(LOG_INFO, "URL:%s", strUrl);
	syslog(LOG_INFO, "get data:%s", strData);
    json_t* all_props = get_all_properties(handle, credentials);

    syslog(LOG_INFO, "ProdShortName: %s", read_property(all_props, "ProdShortName"));
    syslog(LOG_INFO, "Soc: %s", read_property(all_props, "Soc"));
    syslog(LOG_INFO, "SocSerialNumber: %s", read_property(all_props, "SocSerialNumber"));

    json_decref(all_props);
    free(credentials);
    curl_easy_cleanup(handle);
    curl_global_cleanup();
}
