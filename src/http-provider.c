/*
	belle-sip - SIP (RFC3261) library.
    Copyright (C) 2010  Belledonne Communications SARL

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "belle_sip_internal.h"

typedef struct belle_http_channel_context belle_http_channel_context_t;

#define BELLE_HTTP_CHANNEL_CONTEXT(obj) BELLE_SIP_CAST(obj,belle_http_channel_context_t)

static void provider_remove_channel(belle_http_provider_t *obj, belle_sip_channel_t *chan);

struct belle_http_channel_context{
	belle_sip_object_t base;
	belle_http_provider_t *provider;
	belle_sip_list_t *pending_requests;
};

struct belle_http_provider{
	belle_sip_object_t base;
	belle_sip_stack_t *stack;
	char *bind_ip;
	int ai_family;
	belle_sip_list_t *tcp_channels;
	belle_sip_list_t *tls_channels;
	belle_tls_verify_policy_t *verify_ctx;
};

#define BELLE_HTTP_REQUEST_INVOKE_LISTENER(obj,method,arg) \
	obj->listener ? BELLE_SIP_INVOKE_LISTENER_ARG(obj->listener,belle_http_request_listener_t,method,arg) : 0

static int http_channel_context_handle_authentication(belle_http_channel_context_t *ctx, belle_http_request_t *req){
	const char *realm=NULL;
	belle_sip_auth_event_t *ev=NULL;
	belle_http_response_t *resp=belle_http_request_get_response(req);
	const char *username=NULL;
	const char *passwd=NULL;
	int ret=0;
	
	(void)resp;
	if (req->auth_attempt_count>1){
		req->auth_attempt_count=0;
		return -1;
	}
	
	/*find if username, passwd were already supplied in original request uri*/
	if (req->orig_uri){
		username=belle_generic_uri_get_user(req->orig_uri);
		passwd=belle_generic_uri_get_user_password(req->orig_uri);
	}
	
	if (username==NULL || passwd==NULL){
		/*TODO find the realm from the Authentication header*/
		
		ev=belle_sip_auth_event_create((belle_sip_object_t*)ctx->provider,realm,NULL);
		BELLE_HTTP_REQUEST_INVOKE_LISTENER(req,process_auth_requested,ev);
		username=ev->username;
		passwd=ev->passwd;
	}
	
	if (username && passwd){
		/*TODO resubmit the request to the provider with authentication added*/
		req->auth_attempt_count++;
		belle_http_provider_send_request(ctx->provider,req,NULL);
	}else ret=-1;
	
	if (ev) belle_sip_auth_event_destroy(ev);
	return ret;
}
	
static void http_channel_context_handle_response(belle_http_channel_context_t *ctx , belle_sip_channel_t *chan, belle_http_response_t *response){
	belle_http_request_t *req=NULL;
	belle_http_response_event_t ev={0};
	int code;
	belle_sip_header_t *connection;
	/*pop the request matching this response*/
	ctx->pending_requests=belle_sip_list_pop_front(ctx->pending_requests,(void**)&req);
	if (req==NULL){
		belle_sip_error("Receiving http response not matching any request.");
		return;
	}
	connection=belle_sip_message_get_header((belle_sip_message_t *)response,"Connection");
	if (connection && strstr(belle_sip_header_get_unparsed_value(connection),"close")!=NULL)
		chan->about_to_be_closed=TRUE;
	
	belle_http_request_set_response(req,response);
	code=belle_http_response_get_status_code(response);
	if ((code==401 || code==407) && http_channel_context_handle_authentication(ctx,req)==0 ){
		/*nothing to do, the request has been resubmitted with authentication*/
	}else{
		/*else notify the app about the response received*/
		ev.source=(belle_sip_object_t*)ctx->provider;
		ev.request=req;
		ev.response=response;
		BELLE_HTTP_REQUEST_INVOKE_LISTENER(req,process_response,&ev);
	}
	belle_sip_object_unref(req);
}

static int channel_on_event(belle_sip_channel_listener_t *obj, belle_sip_channel_t *chan, unsigned int revents){
	belle_http_channel_context_t *ctx=BELLE_HTTP_CHANNEL_CONTEXT(obj);
	if (revents & BELLE_SIP_EVENT_READ){
		belle_sip_message_t *msg;
		while((msg=belle_sip_channel_pick_message(chan))!=NULL){
			if (msg && BELLE_SIP_OBJECT_IS_INSTANCE_OF(msg,belle_http_response_t)){
				http_channel_context_handle_response(ctx,chan,(belle_http_response_t*)msg);
			}
			belle_sip_object_unref(msg);
		}
	}
	return 0;
}

static int channel_on_auth_requested(belle_sip_channel_listener_t *obj, belle_sip_channel_t *chan, const char* distinguished_name){
	belle_http_channel_context_t *ctx=BELLE_HTTP_CHANNEL_CONTEXT(obj);
	if (BELLE_SIP_IS_INSTANCE_OF(chan,belle_sip_tls_channel_t)) {
		belle_sip_auth_event_t* auth_event = belle_sip_auth_event_create((belle_sip_object_t*)ctx->provider,NULL,NULL);
		belle_sip_tls_channel_t *tls_chan=BELLE_SIP_TLS_CHANNEL(chan);
		belle_http_request_t *req=(belle_http_request_t*)chan->outgoing_messages->data;
		auth_event->mode=BELLE_SIP_AUTH_MODE_TLS;
		belle_sip_auth_event_set_distinguished_name(auth_event,distinguished_name);
		
		BELLE_HTTP_REQUEST_INVOKE_LISTENER(req,process_auth_requested,auth_event);
		belle_sip_tls_channel_set_client_certificates_chain(tls_chan,auth_event->cert);
		belle_sip_tls_channel_set_client_certificate_key(tls_chan,auth_event->key);
		belle_sip_auth_event_destroy(auth_event);
	}
	return 0;
}

static void channel_on_sending(belle_sip_channel_listener_t *obj, belle_sip_channel_t *chan, belle_sip_message_t *msg){
	belle_http_channel_context_t *ctx=BELLE_HTTP_CHANNEL_CONTEXT(obj);
	ctx->pending_requests=belle_sip_list_append(ctx->pending_requests,belle_sip_object_ref(msg));
	
}

static void channel_state_changed(belle_sip_channel_listener_t *obj, belle_sip_channel_t *chan, belle_sip_channel_state_t state){
	belle_http_channel_context_t *ctx=BELLE_HTTP_CHANNEL_CONTEXT(obj);
	switch(state){
		case BELLE_SIP_CHANNEL_INIT:
		case BELLE_SIP_CHANNEL_RES_IN_PROGRESS:
		case BELLE_SIP_CHANNEL_RES_DONE:
		case BELLE_SIP_CHANNEL_CONNECTING:
		case BELLE_SIP_CHANNEL_READY:
		case BELLE_SIP_CHANNEL_RETRY:
			break;
		case BELLE_SIP_CHANNEL_ERROR:
		case BELLE_SIP_CHANNEL_DISCONNECTED:
			if (!chan->force_close) provider_remove_channel(ctx->provider,chan);
			break;
	}
}

static void belle_http_channel_context_uninit(belle_http_channel_context_t *obj){
	belle_sip_list_free_with_data(obj->pending_requests,belle_sip_object_unref);
}

static void on_channel_destroyed(belle_http_channel_context_t *obj, belle_sip_channel_t *chan_being_destroyed){
	belle_sip_channel_remove_listener(chan_being_destroyed,BELLE_SIP_CHANNEL_LISTENER(obj));
	belle_sip_object_unref(obj);
}

/*
 * The http channel context stores pending requests so that they can be matched with response received.
 * It is associated with the channel when the channel is created, and automatically destroyed when the channel is destroyed.
**/
belle_http_channel_context_t * belle_http_channel_context_new(belle_sip_channel_t *chan, belle_http_provider_t *prov){
	belle_http_channel_context_t *obj=belle_sip_object_new(belle_http_channel_context_t);
	obj->provider=prov;
	belle_sip_channel_add_listener(chan,(belle_sip_channel_listener_t*)obj);
	belle_sip_object_weak_ref(chan,(belle_sip_object_destroy_notify_t)on_channel_destroyed,obj);
	return obj;
}

BELLE_SIP_IMPLEMENT_INTERFACE_BEGIN(belle_http_channel_context_t,belle_sip_channel_listener_t)
	channel_state_changed,
	channel_on_event,
	channel_on_sending,
	channel_on_auth_requested
BELLE_SIP_IMPLEMENT_INTERFACE_END

BELLE_SIP_DECLARE_IMPLEMENTED_INTERFACES_1(belle_http_channel_context_t,belle_sip_channel_listener_t);
BELLE_SIP_INSTANCIATE_VPTR(belle_http_channel_context_t,belle_sip_object_t,belle_http_channel_context_uninit,NULL,NULL,FALSE);

static void http_provider_uninit(belle_http_provider_t *obj){
	belle_sip_message("http provider destroyed.");
	belle_sip_free(obj->bind_ip);
	belle_sip_list_for_each(obj->tcp_channels,(void (*)(void*))belle_sip_channel_force_close);
	belle_sip_list_free_with_data(obj->tcp_channels,belle_sip_object_unref);
	belle_sip_list_for_each(obj->tls_channels,(void (*)(void*))belle_sip_channel_force_close);
	belle_sip_list_free_with_data(obj->tls_channels,belle_sip_object_unref);
	belle_sip_object_unref(obj->verify_ctx);
}

BELLE_SIP_DECLARE_NO_IMPLEMENTED_INTERFACES(belle_http_provider_t);
BELLE_SIP_INSTANCIATE_VPTR(belle_http_provider_t,belle_sip_object_t,http_provider_uninit,NULL,NULL,FALSE);

belle_http_provider_t *belle_http_provider_new(belle_sip_stack_t *s, const char *bind_ip){
	belle_http_provider_t *p=belle_sip_object_new(belle_http_provider_t);
	p->stack=s;
	p->bind_ip=belle_sip_strdup(bind_ip);
	p->ai_family=strchr(p->bind_ip,':') ? AF_INET6 : AF_INET;
	p->verify_ctx=belle_tls_verify_policy_new();
	return p;
}

static void split_request_url(belle_http_request_t *req){
	belle_generic_uri_t *uri=belle_http_request_get_uri(req);
	belle_generic_uri_t *new_uri;
	char *host_value;
	const char *path;
	
	if (belle_generic_uri_get_host(uri)==NULL && req->orig_uri!=NULL) return;/*already processed request uri*/
	path=belle_generic_uri_get_path(uri);
	if (path==NULL) path="/";
	new_uri=belle_generic_uri_new();

	belle_generic_uri_set_path(new_uri,path);
	if (belle_generic_uri_get_port(uri)>0)
		host_value=belle_sip_strdup_printf("%s:%i",belle_generic_uri_get_host(uri),belle_generic_uri_get_port(uri));
	else 
		host_value=belle_sip_strdup(belle_generic_uri_get_host(uri));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),belle_sip_header_create("Host",host_value));
	belle_sip_free(host_value);
	SET_OBJECT_PROPERTY(req,orig_uri,uri);
	belle_http_request_set_uri(req,new_uri);
}

static void fix_request(belle_http_request_t *req){
	unsigned int size=belle_sip_message_get_body_size((belle_sip_message_t*)req);
	belle_sip_header_content_length_t *ctlen=belle_sip_message_get_header_by_type(req, belle_sip_header_content_length_t);
	if (size>0 && !ctlen){
		belle_sip_message_add_header((belle_sip_message_t*)req,(belle_sip_header_t*)belle_sip_header_content_length_create((int)size));
	}
}

static belle_sip_list_t **provider_get_channels(belle_http_provider_t *obj, const char *transport_name){
	if (strcasecmp(transport_name,"tcp")==0) return &obj->tcp_channels;
	else if (strcasecmp(transport_name,"tls")==0) return &obj->tls_channels;
	else{
		belle_sip_error("belle_http_provider_send_request(): unsupported transport %s",transport_name);
		return NULL;
	}
}

static void provider_remove_channel(belle_http_provider_t *obj, belle_sip_channel_t *chan){
	belle_sip_list_t **channels=provider_get_channels(obj,belle_sip_channel_get_transport_name(chan));
	*channels=belle_sip_list_remove(*channels,chan);
	belle_sip_message("channel [%p] removed from http provider.",obj);
	belle_sip_object_unref(chan);
}

int belle_http_provider_send_request(belle_http_provider_t *obj, belle_http_request_t *req, belle_http_request_listener_t *listener){
	belle_sip_channel_t *chan;
	belle_sip_hop_t *hop=belle_sip_hop_new_from_generic_uri(req->orig_uri ? req->orig_uri : req->req_uri);
	belle_sip_list_t **channels=provider_get_channels(obj,hop->transport);
	
	if (listener) belle_http_request_set_listener(req,listener);
	
	chan=belle_sip_channel_find_from_list(*channels,obj->ai_family, hop);
	
	if (!chan){
		if (strcasecmp(hop->transport,"tcp")==0){
			chan=belle_sip_stream_channel_new_client(obj->stack,obj->bind_ip,0,hop->cname,hop->host,hop->port);
		}
#ifdef HAVE_POLARSSL
		else if (strcasecmp(hop->transport,"tls")==0){
			chan=belle_sip_channel_new_tls(obj->stack,obj->verify_ctx,obj->bind_ip,0,hop->cname,hop->host,hop->port);
		}
#endif
		if (!chan){
			belle_sip_error("belle_http_provider_send_request(): cannot create channel for [%s:%s:%i]",hop->transport,hop->cname,hop->port);
			belle_sip_object_unref(hop);
			return -1;
		}
		belle_http_channel_context_new(chan,obj);
		*channels=belle_sip_list_prepend(*channels,chan);
	}
	belle_sip_object_unref(hop);
	split_request_url(req);
	fix_request(req);
	belle_sip_channel_queue_message(chan,BELLE_SIP_MESSAGE(req));
	return 0;
}

int belle_http_provider_set_tls_verify_policy(belle_http_provider_t *obj, belle_tls_verify_policy_t *verify_ctx){
	SET_OBJECT_PROPERTY(obj,verify_ctx,verify_ctx);
	return 0;
}

