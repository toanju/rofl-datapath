/*
 * cchannel.cpp
 *
 *  Created on: 31.12.2013
 *      Author: andreas
 */

#include "crofsock.h"

using namespace rofl::openflow;

crofsock::crofsock(
		crofsock_env *env,
		int sd) :
				env(env),
				socket(this, sd),
				fragment((cmemory*)0),
				msg_bytes_read(0)
{

}



crofsock::crofsock(
		crofsock_env *env,
		int domain,
		int type,
		int protocol,
		rofl::caddress const& ra) :
				env(env),
				socket(this, domain, type, protocol, /*backlog=*/10),
				fragment((cmemory*)0),
				msg_bytes_read(0)
{
	socket.cconnect(ra, caddress(AF_INET, "0.0.0.0", 0), domain, type, protocol);
}



crofsock::~crofsock()
{

}



void
crofsock::close()
{
	socket.cclose();
}



void
crofsock::handle_accepted(
		csocket *socket,
		int newsd,
		caddress const& ra)
{
	logging::info << "[rofl][sock] connection accepted:" << std::endl << *this;
	// this should never happen, as passively opened sockets are handled outside of crofsock
}



void
crofsock::handle_connected(
		csocket *socket,
		int sd)
{
	logging::info << "[rofl][sock] connection established:" << std::endl << *this;
	env->handle_connected(this);
}



void
crofsock::handle_connect_refused(
		csocket *socket,
		int sd)
{
	logging::info << "[rofl][sock] connection failed:" << std::endl << *this;
	env->handle_connect_refused(this);
}



void
crofsock::handle_closed(
			csocket *socket,
			int sd)
{
	logging::info << "[rofl][sock] connection closed:" << std::endl << *this;
	env->handle_closed(this);
}



void
crofsock::handle_read(
		csocket *socket,
		int sd)
{
	int rc = 0;

	cmemory *mem = (cmemory*)0;
	try {

		if (0 == fragment) {
			mem = new cmemory(sizeof(struct openflow::ofp_header));
			msg_bytes_read = 0;
		} else {
			mem = fragment;
		}

		while (true) {

			uint16_t msg_len = 0;

			// how many bytes do we have to read?
			if (msg_bytes_read < sizeof(struct openflow::ofp_header)) {
				msg_len = sizeof(struct openflow::ofp_header);
			} else {
				struct openflow::ofp_header *ofh_header = (struct openflow::ofp_header*)mem->somem();
				msg_len = be16toh(ofh_header->length);
			}

			// resize msg buffer, if necessary
			if (mem->memlen() < msg_len) {
				mem->resize(msg_len);
			}

			// read from socket more bytes, at most "msg_len - msg_bytes_read"
			rc = socket->recv((void*)(mem->somem() + msg_bytes_read), msg_len - msg_bytes_read);

			if (rc < 0) { // error occured (or non-blocking)
				switch(errno) {
				case EAGAIN: {
					fragment = mem;	// more bytes are needed, store pointer to msg in "fragment"
				} return;
				case ECONNREFUSED: {
					env->handle_connect_refused(this);
				} return;
				case ECONNRESET:
				default: {
					logging::error << "[rofl][sock] error reading from socket descriptor:" << sd
							<< " " << eSysCall() << ", closing endpoint." << std::endl;
					handle_closed(socket, sd);
				} return;
				}
			}
			else if (rc == 0) // socket was closed
			{
				//rfds.erase(fd);
				logging::info << "[rofl][sock] peer closed connection." << std::endl << *this;

				if (mem) {
					delete mem; fragment = (cmemory*)0;
				}

				//socket->cclose();
				env->handle_closed(this);
				return;
			}
			else // rc > 0, // some bytes were received, check for completeness of packet
			{
				msg_bytes_read += rc;

				// minimum message length received, check completeness of message
				if (mem->memlen() >= sizeof(struct openflow::ofp_header)) {
					struct openflow::ofp_header *ofh_header = (struct openflow::ofp_header*)mem->somem();
					uint16_t msg_len = be16toh(ofh_header->length);

					// ok, message was received completely
					if (msg_len == msg_bytes_read) {
						fragment = (cmemory*)0;
						parse_message(mem);
						return;
					}
				}
			}
		}

	} catch (RoflException& e) {

		logging::warn << "[rofl][sock] dropping invalid message " << *mem << std::endl;

		if (mem) {
			delete mem; fragment = (cmemory*)0;
		}
		// handle_closed(socket, sd);
	}

}



rofl::csocket&
crofsock::get_socket()
{
	return socket;
}



void
crofsock::send_message(
		cofmsg *msg)
{
	if (not socket.is_connected()) {
		delete msg; return;
	}

	log_message(std::string("queueing message for sending:"), *msg);

	RwLock(outqueue_rwlock, RwLock::RWLOCK_WRITE);

	outqueue.push_back(msg);

	notify(CROFSOCK_EVENT_WAKEUP);
}



void
crofsock::send_from_queue()
{
	RwLock(outqueue_rwlock, RwLock::RWLOCK_WRITE);

	while (not outqueue.empty()) {

		cofmsg *msg = outqueue.front();
		outqueue.pop_front();

		cmemory *mem = new cmemory(msg->length());

		msg->pack(mem->somem(), mem->memlen());

		delete msg;

		socket.send(mem);
	}
}



void
crofsock::handle_event(
		cevent const &ev)
{
	switch (ev.cmd) {
	case CROFSOCK_EVENT_WAKEUP: {
		send_from_queue();
	} break;
	default:
		logging::error << "[rofl][sock] unknown event type:" << (int)ev.cmd << std::endl;
	}
}



void
crofsock::parse_message(
		cmemory *mem)
{
	cofmsg *msg = (cofmsg*)0;
	try {
		assert(NULL != mem);

		struct openflow::ofp_header* ofh_header = (struct openflow::ofp_header*)mem->somem();

		switch (ofh_header->version) {
		case openflow10::OFP_VERSION: parse_of10_message(mem, &msg); break;
		case openflow12::OFP_VERSION: parse_of12_message(mem, &msg); break;
		case openflow13::OFP_VERSION: parse_of13_message(mem, &msg); break;
		default: msg = new cofmsg(mem); break;
		}

		log_message(std::string("received message:"), *msg);

		env->recv_message(this, msg);

	} catch (eBadRequestBadType& e) {

		logging::error << "[rofl][sock] eBadRequestBadType " << std::endl;
		if (msg) delete msg;

	} catch (RoflException& e) {

		logging::error << "[rofl][sock] RoflException " << std::endl;
		if (msg) delete msg;

	}
}



void
crofsock::parse_of10_message(cmemory *mem, cofmsg **pmsg)
{
	struct openflow::ofp_header* ofh_header = (struct openflow::ofp_header*)mem->somem();

	switch (ofh_header->type) {
	case openflow10::OFPT_HELLO: {
		(*pmsg = new cofmsg_hello(mem))->validate();
	} break;

	case openflow10::OFPT_ECHO_REQUEST: {
		(*pmsg = new cofmsg_echo_request(mem))->validate();
	} break;
	case openflow10::OFPT_ECHO_REPLY: {
		(*pmsg = new cofmsg_echo_reply(mem))->validate();
	} break;

	case openflow10::OFPT_VENDOR: {
		(*pmsg = new cofmsg_experimenter(mem))->validate();
	} break;

	case openflow10::OFPT_FEATURES_REQUEST:	{
		(*pmsg = new cofmsg_features_request(mem))->validate();
	} break;
	case openflow10::OFPT_FEATURES_REPLY: {
		(*pmsg = new cofmsg_features_reply(mem))->validate();
	} break;

	case openflow10::OFPT_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_get_config_request(mem))->validate();
	} break;
	case openflow10::OFPT_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_get_config_reply(mem))->validate();
	} break;
	case openflow10::OFPT_SET_CONFIG: {
		(*pmsg = new cofmsg_set_config(mem))->validate();
	} break;

	case openflow10::OFPT_PACKET_OUT: {
		(*pmsg = new cofmsg_packet_out(mem))->validate();
	} break;
	case openflow10::OFPT_PACKET_IN: {
		(*pmsg = new cofmsg_packet_in(mem))->validate();
	} break;

	case openflow10::OFPT_FLOW_MOD: {
		(*pmsg = new cofmsg_flow_mod(mem))->validate();
		dynamic_cast<cofmsg_flow_mod*>( *pmsg )->get_match().check_prerequisites();
	} break;
	case openflow10::OFPT_FLOW_REMOVED: {
		(*pmsg = new cofmsg_flow_removed(mem))->validate();
	} break;

	case openflow10::OFPT_PORT_MOD: {
		(*pmsg = new cofmsg_port_mod(mem))->validate();
	} break;
	case openflow10::OFPT_PORT_STATUS: {
		(*pmsg = new cofmsg_port_status(mem))->validate();
	} break;

	case openflow10::OFPT_STATS_REQUEST: {
		if (mem->memlen() < sizeof(struct openflow10::ofp_stats_request)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow10::ofp_stats_request*)mem->somem())->type);

		switch (stats_type) {
		case openflow10::OFPST_DESC: {
			(*pmsg = new cofmsg_desc_stats_request(mem))->validate();
		} break;
		case openflow10::OFPST_FLOW: {
			(*pmsg = new cofmsg_flow_stats_request(mem))->validate();
		} break;
		case openflow10::OFPST_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_request(mem))->validate();
		} break;
		case openflow10::OFPST_TABLE: {
			(*pmsg = new cofmsg_table_stats_request(mem))->validate();
		} break;
		case openflow10::OFPST_PORT: {
			(*pmsg = new cofmsg_port_stats_request(mem))->validate();
		} break;
		case openflow10::OFPST_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_request(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_request(mem))->validate();
		} break;
		}

	} break;
	case openflow10::OFPT_STATS_REPLY: {
		if (mem->memlen() < sizeof(struct openflow10::ofp_stats_reply)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow10::ofp_stats_reply*)mem->somem())->type);

		switch (stats_type) {
		case openflow10::OFPST_DESC: {
			(*pmsg = new cofmsg_desc_stats_reply(mem))->validate();
		} break;
		case openflow10::OFPST_FLOW: {
			(*pmsg = new cofmsg_flow_stats_reply(mem))->validate();
		} break;
		case openflow10::OFPST_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_reply(mem))->validate();
		} break;
		case openflow10::OFPST_TABLE: {
			(*pmsg = new cofmsg_table_stats_reply(mem))->validate();
		} break;
		case openflow10::OFPST_PORT: {
			(*pmsg = new cofmsg_port_stats_reply(mem))->validate();
		} break;
		case openflow10::OFPST_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_reply(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_reply(mem))->validate();
		} break;
		}

	} break;

	case openflow10::OFPT_BARRIER_REQUEST: {
		(*pmsg = new cofmsg_barrier_request(mem))->validate();
	} break;
	case openflow10::OFPT_BARRIER_REPLY: {
		(*pmsg = new cofmsg_barrier_reply(mem))->validate();
	} break;
	case openflow10::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_queue_get_config_request(mem))->validate();
	} break;
	case openflow10::OFPT_QUEUE_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_queue_get_config_reply(mem))->validate();
	} break;

	default: {
		(*pmsg = new cofmsg(mem))->validate();
		logging::warn << "[rofl][sock] dropping unknown message " << **pmsg << std::endl;
		throw eBadRequestBadType();
	} break;
	}
}



void
crofsock::parse_of12_message(cmemory *mem, cofmsg **pmsg)
{
	struct openflow::ofp_header* ofh_header = (struct openflow::ofp_header*)mem->somem();

	switch (ofh_header->type) {
	case openflow12::OFPT_HELLO: {
		(*pmsg = new cofmsg_hello(mem))->validate();
	} break;

	case openflow12::OFPT_ECHO_REQUEST: {
		(*pmsg = new cofmsg_echo_request(mem))->validate();
	} break;
	case openflow12::OFPT_ECHO_REPLY: {
		(*pmsg = new cofmsg_echo_reply(mem))->validate();
	} break;

	case openflow12::OFPT_EXPERIMENTER:	{
		(*pmsg = new cofmsg_experimenter(mem))->validate();
	} break;

	case openflow12::OFPT_FEATURES_REQUEST:	{
		(*pmsg = new cofmsg_features_request(mem))->validate();
	} break;
	case openflow12::OFPT_FEATURES_REPLY: {
		(*pmsg = new cofmsg_features_reply(mem))->validate();
	} break;

	case openflow12::OFPT_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_get_config_request(mem))->validate();
	} break;
	case openflow12::OFPT_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_get_config_reply(mem))->validate();
	} break;
	case openflow12::OFPT_SET_CONFIG: {
		(*pmsg = new cofmsg_set_config(mem))->validate();
	} break;

	case openflow12::OFPT_PACKET_OUT: {
		(*pmsg = new cofmsg_packet_out(mem))->validate();
	} break;
	case openflow12::OFPT_PACKET_IN: {
		(*pmsg = new cofmsg_packet_in(mem))->validate();
	} break;

	case openflow12::OFPT_FLOW_MOD: {
		(*pmsg = new cofmsg_flow_mod(mem))->validate();
		dynamic_cast<cofmsg_flow_mod*>( *pmsg )->get_match().check_prerequisites();
	} break;
	case openflow12::OFPT_FLOW_REMOVED: {
		(*pmsg = new cofmsg_flow_removed(mem))->validate();
	} break;

	case openflow12::OFPT_GROUP_MOD: {
		(*pmsg = new cofmsg_group_mod(mem))->validate();
	} break;

	case openflow12::OFPT_PORT_MOD: {
		(*pmsg = new cofmsg_port_mod(mem))->validate();
	} break;
	case openflow12::OFPT_PORT_STATUS: {
		(*pmsg = new cofmsg_port_status(mem))->validate();
	} break;

	case openflow12::OFPT_TABLE_MOD: {
		(*pmsg = new cofmsg_table_mod(mem))->validate();
	} break;

	case openflow12::OFPT_STATS_REQUEST: {

		if (mem->memlen() < sizeof(struct openflow12::ofp_stats_request)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow12::ofp_stats_request*)mem->somem())->type);

		switch (stats_type) {
		case openflow12::OFPST_DESC: {
			(*pmsg = new cofmsg_desc_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_FLOW: {
			(*pmsg = new cofmsg_flow_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_TABLE: {
			(*pmsg = new cofmsg_table_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_PORT: {
			(*pmsg = new cofmsg_port_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP: {
			(*pmsg = new cofmsg_group_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP_DESC: {
			(*pmsg = new cofmsg_group_desc_stats_request(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP_FEATURES: {
			(*pmsg = new cofmsg_group_features_stats_request(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_request(mem))->validate();
		} break;
		}

	} break;
	case openflow12::OFPT_STATS_REPLY: {
		if (mem->memlen() < sizeof(struct openflow12::ofp_stats_reply)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow12::ofp_stats_reply*)mem->somem())->type);

		switch (stats_type) {
		case openflow12::OFPST_DESC: {
			(*pmsg = new cofmsg_desc_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_FLOW: {
			(*pmsg = new cofmsg_flow_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_TABLE: {
			(*pmsg = new cofmsg_table_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_PORT: {
			(*pmsg = new cofmsg_port_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP: {
			(*pmsg = new cofmsg_group_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP_DESC: {
			(*pmsg = new cofmsg_group_desc_stats_reply(mem))->validate();
		} break;
		case openflow12::OFPST_GROUP_FEATURES: {
			(*pmsg = new cofmsg_group_features_stats_reply(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_reply(mem))->validate();
		} break;
		}

	} break;

	case openflow12::OFPT_BARRIER_REQUEST: {
		(*pmsg = new cofmsg_barrier_request(mem))->validate();
	} break;
	case openflow12::OFPT_BARRIER_REPLY: {
		(*pmsg = new cofmsg_barrier_reply(mem))->validate();
	} break;

	case openflow12::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_queue_get_config_request(mem))->validate();
	} break;
	case openflow12::OFPT_QUEUE_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_queue_get_config_reply(mem))->validate();
	} break;

	case openflow12::OFPT_ROLE_REQUEST: {
		(*pmsg = new cofmsg_role_request(mem))->validate();
	} break;
	case openflow12::OFPT_ROLE_REPLY: {
		(*pmsg = new cofmsg_role_reply(mem))->validate();
	} break;

	case openflow12::OFPT_GET_ASYNC_REQUEST: {
    	(*pmsg = new cofmsg_get_async_config_request(mem))->validate();
    } break;
	case openflow12::OFPT_GET_ASYNC_REPLY: {
		(*pmsg = new cofmsg_get_async_config_reply(mem))->validate();
	} break;
	case openflow12::OFPT_SET_ASYNC: {
    	(*pmsg = new cofmsg_set_async_config(mem))->validate();
    } break;

	default: {
		(*pmsg = new cofmsg(mem))->validate();
		logging::warn << "[rofl][sock] dropping unknown message " << **pmsg << std::endl;
		throw eBadRequestBadType();
	} return;
	}
}



void
crofsock::parse_of13_message(cmemory *mem, cofmsg **pmsg)
{
	struct openflow::ofp_header* ofh_header = (struct openflow::ofp_header*)mem->somem();

	switch (ofh_header->type) {
	case openflow13::OFPT_HELLO: {
		(*pmsg = new cofmsg_hello(mem))->validate();
		logging::debug << dynamic_cast<cofmsg_hello&>( **pmsg );
	} break;

	case openflow13::OFPT_ECHO_REQUEST: {
		(*pmsg = new cofmsg_echo_request(mem))->validate();
	} break;
	case openflow13::OFPT_ECHO_REPLY: {
		(*pmsg = new cofmsg_echo_reply(mem))->validate();
	} break;

	case openflow13::OFPT_EXPERIMENTER:	{
		(*pmsg = new cofmsg_experimenter(mem))->validate();
	} break;

	case openflow13::OFPT_FEATURES_REQUEST:	{
		(*pmsg = new cofmsg_features_request(mem))->validate();
	} break;
	case openflow13::OFPT_FEATURES_REPLY: {
		(*pmsg = new cofmsg_features_reply(mem))->validate();
	} break;

	case openflow13::OFPT_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_get_config_request(mem))->validate();
	} break;
	case openflow13::OFPT_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_get_config_reply(mem))->validate();
	} break;
	case openflow13::OFPT_SET_CONFIG: {
		(*pmsg = new cofmsg_set_config(mem))->validate();
	} break;

	case openflow13::OFPT_PACKET_OUT: {
		(*pmsg = new cofmsg_packet_out(mem))->validate();
	} break;
	case openflow13::OFPT_PACKET_IN: {
		(*pmsg = new cofmsg_packet_in(mem))->validate();
	} break;

	case openflow13::OFPT_FLOW_MOD: {
		(*pmsg = new cofmsg_flow_mod(mem))->validate();
		dynamic_cast<cofmsg_flow_mod*>( *pmsg )->get_match().check_prerequisites();
	} break;
	case openflow13::OFPT_FLOW_REMOVED: {
		(*pmsg = new cofmsg_flow_removed(mem))->validate();
	} break;

	case openflow13::OFPT_GROUP_MOD: {
		(*pmsg = new cofmsg_group_mod(mem))->validate();
	} break;

	case openflow13::OFPT_PORT_MOD: {
		(*pmsg = new cofmsg_port_mod(mem))->validate();
	} break;
	case openflow13::OFPT_PORT_STATUS: {
		(*pmsg = new cofmsg_port_status(mem))->validate();
	} break;

	case openflow13::OFPT_TABLE_MOD: {
		(*pmsg = new cofmsg_table_mod(mem))->validate();
	} break;

	case openflow13::OFPT_MULTIPART_REQUEST: {

		if (mem->memlen() < sizeof(struct openflow13::ofp_multipart_request)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow13::ofp_multipart_request*)mem->somem())->type);

		switch (stats_type) {
		case openflow13::OFPMP_DESC: {
			(*pmsg = new cofmsg_desc_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_FLOW: {
			(*pmsg = new cofmsg_flow_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_TABLE: {
			(*pmsg = new cofmsg_table_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_PORT_DESC: {
			(*pmsg = new cofmsg_port_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP: {
			(*pmsg = new cofmsg_group_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP_DESC: {
			(*pmsg = new cofmsg_group_desc_stats_request(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP_FEATURES: {
			(*pmsg = new cofmsg_group_features_stats_request(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_request(mem))->validate();
		} break;
		}

	} break;
	case openflow13::OFPT_MULTIPART_REPLY: {
		if (mem->memlen() < sizeof(struct openflow13::ofp_multipart_reply)) {
			*pmsg = new cofmsg(mem);
			throw eBadSyntaxTooShort();
		}
		uint16_t stats_type = be16toh(((struct openflow13::ofp_multipart_reply*)mem->somem())->type);

		switch (stats_type) {
		case openflow13::OFPMP_DESC: {
			(*pmsg = new cofmsg_desc_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_FLOW: {
			(*pmsg = new cofmsg_flow_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_AGGREGATE: {
			(*pmsg = new cofmsg_aggr_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_TABLE: {
			(*pmsg = new cofmsg_table_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_PORT_DESC: {
			(*pmsg = new cofmsg_port_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_QUEUE: {
			(*pmsg = new cofmsg_queue_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP: {
			(*pmsg = new cofmsg_group_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP_DESC: {
			(*pmsg = new cofmsg_group_desc_stats_reply(mem))->validate();
		} break;
		case openflow13::OFPMP_GROUP_FEATURES: {
			(*pmsg = new cofmsg_group_features_stats_reply(mem))->validate();
		} break;
		// TODO: experimenter statistics
		default: {
			(*pmsg = new cofmsg_stats_reply(mem))->validate();
		} break;
		}

	} break;

	case openflow13::OFPT_BARRIER_REQUEST: {
		(*pmsg = new cofmsg_barrier_request(mem))->validate();
	} break;
	case openflow13::OFPT_BARRIER_REPLY: {
		(*pmsg = new cofmsg_barrier_reply(mem))->validate();
	} break;

	case openflow13::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		(*pmsg = new cofmsg_queue_get_config_request(mem))->validate();
	} break;
	case openflow13::OFPT_QUEUE_GET_CONFIG_REPLY: {
		(*pmsg = new cofmsg_queue_get_config_reply(mem))->validate();
	} break;

	case openflow13::OFPT_ROLE_REQUEST: {
		(*pmsg = new cofmsg_role_request(mem))->validate();
	} break;
	case openflow13::OFPT_ROLE_REPLY: {
		(*pmsg = new cofmsg_role_reply(mem))->validate();
	} break;

	case openflow13::OFPT_GET_ASYNC_REQUEST: {
    	(*pmsg = new cofmsg_get_async_config_request(mem))->validate();
    } break;
	case openflow13::OFPT_GET_ASYNC_REPLY: {
		(*pmsg = new cofmsg_get_async_config_reply(mem))->validate();
	} break;
	case openflow13::OFPT_SET_ASYNC: {
    	(*pmsg = new cofmsg_set_async_config(mem))->validate();
    } break;

	default: {
		(*pmsg = new cofmsg(mem))->validate();
		logging::warn << "[rofl][sock] dropping unknown message " << **pmsg << std::endl;
		throw eBadRequestBadType();
	} return;
	}
}








void
crofsock::log_message(
		std::string const& text, cofmsg const& msg)
{
	logging::debug << "[rofl][sock] " << text << std::endl;

	switch (msg.get_version()) {
	case rofl::openflow10::OFP_VERSION: log_of10_message(msg); break;
	case rofl::openflow12::OFP_VERSION: log_of12_message(msg); break;
	case rofl::openflow13::OFP_VERSION: log_of13_message(msg); break;
	default: logging::debug << "[rolf][sock] unknown OFP version found in msg" << std::endl << msg; break;
	}
}






void
crofsock::log_of10_message(
		cofmsg const& msg)
{
	switch (msg.get_type()) {
	case openflow10::OFPT_HELLO: {
		logging::debug << dynamic_cast<cofmsg_hello const&>( msg );
	} break;
	case openflow10::OFPT_ECHO_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_echo_request const&>( msg );
	} break;
	case openflow10::OFPT_ECHO_REPLY: {
		logging::debug << dynamic_cast<cofmsg_echo_reply const&>( msg );
	} break;
	case openflow10::OFPT_VENDOR: {
		logging::debug << dynamic_cast<cofmsg_experimenter const&>( msg );
	} break;
	case openflow10::OFPT_FEATURES_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_features_request const&>( msg );
	} break;
	case openflow10::OFPT_FEATURES_REPLY: {
		logging::debug << dynamic_cast<cofmsg_features_reply const&>( msg );
	} break;
	case openflow10::OFPT_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_get_config_request const&>( msg );
	} break;
	case openflow10::OFPT_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_get_config_reply const&>( msg );
	} break;
	case openflow10::OFPT_SET_CONFIG: {
		logging::debug << dynamic_cast<cofmsg_set_config const&>( msg );
	} break;
	case openflow10::OFPT_PACKET_OUT: {
		logging::debug << dynamic_cast<cofmsg_packet_out const&>( msg );
	} break;
	case openflow10::OFPT_PACKET_IN: {
		logging::debug << dynamic_cast<cofmsg_packet_in const&>( msg );
	} break;
	case openflow10::OFPT_FLOW_MOD: {
		logging::debug << dynamic_cast<cofmsg_flow_mod const&>( msg );
	} break;
	case openflow10::OFPT_FLOW_REMOVED: {
		logging::debug << dynamic_cast<cofmsg_flow_removed const&>( msg );
	} break;
	case openflow10::OFPT_PORT_MOD: {
		logging::debug << dynamic_cast<cofmsg_port_mod const&>( msg );
	} break;
	case openflow10::OFPT_PORT_STATUS: {
		logging::debug << dynamic_cast<cofmsg_port_status const&>( msg );
	} break;
	case openflow10::OFPT_STATS_REQUEST: {
		cofmsg_stats_request const& stats = dynamic_cast<cofmsg_stats_request const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow10::OFPST_DESC: {
			logging::debug << dynamic_cast<cofmsg_desc_stats_request const&>( msg );
		} break;
		case openflow10::OFPST_FLOW: {
			logging::debug << dynamic_cast<cofmsg_flow_stats_request const&>( msg );
		} break;
		case openflow10::OFPST_AGGREGATE: {
			logging::debug << dynamic_cast<cofmsg_aggr_stats_request const&>( msg );
		} break;
		case openflow10::OFPST_TABLE: {
			logging::debug << dynamic_cast<cofmsg_table_stats_request const&>( msg );
		} break;
		case openflow10::OFPST_PORT: {
			logging::debug << dynamic_cast<cofmsg_port_stats_request const&>( msg );
		} break;
		case openflow10::OFPST_QUEUE: {
			logging::debug << dynamic_cast<cofmsg_queue_stats_request const&>( msg );
		} break;
		// TODO: experimenter statistics
		default:
			logging::debug << dynamic_cast<cofmsg_stats_request const&>( msg ); break;
		}

	} break;
	case openflow10::OFPT_STATS_REPLY: {
		cofmsg_stats_reply const& stats = dynamic_cast<cofmsg_stats_reply const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow10::OFPST_DESC: {
			logging::debug << dynamic_cast<cofmsg_desc_stats_reply const&>( msg );
		} break;
		case openflow10::OFPST_FLOW: {
			logging::debug << dynamic_cast<cofmsg_flow_stats_reply const&>( msg );
		} break;
		case openflow10::OFPST_AGGREGATE: {
			logging::debug << dynamic_cast<cofmsg_aggr_stats_reply const&>( msg );
		} break;
		case openflow10::OFPST_TABLE: {
			logging::debug << dynamic_cast<cofmsg_table_stats_reply const&>( msg );
		} break;
		case openflow10::OFPST_PORT: {
			logging::debug << dynamic_cast<cofmsg_port_stats_reply const&>( msg );
		} break;
		case openflow10::OFPST_QUEUE: {
			logging::debug << dynamic_cast<cofmsg_queue_stats_reply const&>( msg );
		} break;
		// TODO: experimenter statistics
		default: {
			logging::debug << dynamic_cast<cofmsg_stats_reply const&>( msg );
		} break;
		}

	} break;
	case openflow10::OFPT_BARRIER_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_barrier_request const&>( msg );
	} break;
	case openflow10::OFPT_BARRIER_REPLY: {
		logging::debug << dynamic_cast<cofmsg_barrier_reply const&>( msg );
	} break;
	case openflow10::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_request const&>( msg );
	} break;
	case openflow10::OFPT_QUEUE_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_reply const&>( msg );
	} break;
	default: {
		logging::debug << "[rofl][sock]  unknown message " << msg << std::endl;
	} break;
	}
}



void
crofsock::log_of12_message(
		cofmsg const& msg)
{
	switch (msg.get_type()) {
	case openflow12::OFPT_HELLO: {
		logging::debug << dynamic_cast<cofmsg_hello const&>( msg );
	} break;
	case openflow12::OFPT_ECHO_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_echo_request const&>( msg );
	} break;
	case openflow12::OFPT_ECHO_REPLY: {
		logging::debug << dynamic_cast<cofmsg_echo_reply const&>( msg );
	} break;
	case openflow12::OFPT_EXPERIMENTER:	{
		logging::debug << dynamic_cast<cofmsg_experimenter const&>( msg );
	} break;
	case openflow12::OFPT_FEATURES_REQUEST:	{
		logging::debug << dynamic_cast<cofmsg_features_request const&>( msg );
	} break;
	case openflow12::OFPT_FEATURES_REPLY: {
		logging::debug << dynamic_cast<cofmsg_features_reply const&>( msg );
	} break;
	case openflow12::OFPT_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_get_config_request const&>( msg );
	} break;
	case openflow12::OFPT_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_get_config_reply const&>( msg );
	} break;
	case openflow12::OFPT_SET_CONFIG: {
		logging::debug << dynamic_cast<cofmsg_set_config const&>( msg );
	} break;
	case openflow12::OFPT_PACKET_OUT: {
		logging::debug << dynamic_cast<cofmsg_packet_out const&>( msg );
	} break;
	case openflow12::OFPT_PACKET_IN: {
		logging::debug << dynamic_cast<cofmsg_packet_in const&>( msg );
	} break;
	case openflow12::OFPT_FLOW_MOD: {
		logging::debug << dynamic_cast<cofmsg_flow_mod const&>( msg );
	} break;
	case openflow12::OFPT_FLOW_REMOVED: {
		logging::debug << dynamic_cast<cofmsg_flow_removed const&>( msg );
	} break;
	case openflow12::OFPT_GROUP_MOD: {
		logging::debug << dynamic_cast<cofmsg_group_mod const&>( msg );
	} break;
	case openflow12::OFPT_PORT_MOD: {
		logging::debug << dynamic_cast<cofmsg_port_mod const&>( msg );
	} break;
	case openflow12::OFPT_PORT_STATUS: {
		logging::debug << dynamic_cast<cofmsg_port_status const&>( msg );
	} break;
	case openflow12::OFPT_TABLE_MOD: {
		logging::debug << dynamic_cast<cofmsg_table_mod const&>( msg );
	} break;
	case openflow12::OFPT_STATS_REQUEST: {
		cofmsg_stats_request const& stats = dynamic_cast<cofmsg_stats_request const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow12::OFPST_DESC: {
			logging::debug << dynamic_cast<cofmsg_desc_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_FLOW: {
			logging::debug << dynamic_cast<cofmsg_flow_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_AGGREGATE: {
			logging::debug << dynamic_cast<cofmsg_aggr_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_TABLE: {
			logging::debug << dynamic_cast<cofmsg_table_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_PORT: {
			logging::debug << dynamic_cast<cofmsg_port_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_QUEUE: {
			logging::debug << dynamic_cast<cofmsg_queue_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_GROUP: {
			logging::debug << dynamic_cast<cofmsg_group_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_GROUP_DESC: {
			logging::debug << dynamic_cast<cofmsg_group_desc_stats_request const&>( msg );
		} break;
		case openflow12::OFPST_GROUP_FEATURES: {
			logging::debug << dynamic_cast<cofmsg_group_features_stats_request const&>( msg );
		} break;
		// TODO: experimenter statistics
		default: {
			logging::debug << dynamic_cast<cofmsg_stats_request const&>( msg );
		} break;
		}

	} break;
	case openflow12::OFPT_STATS_REPLY: {
		cofmsg_stats_reply const& stats = dynamic_cast<cofmsg_stats_reply const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow12::OFPST_DESC: {
			logging::debug << dynamic_cast<cofmsg_desc_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_FLOW: {
			logging::debug << dynamic_cast<cofmsg_flow_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_AGGREGATE: {
			logging::debug << dynamic_cast<cofmsg_aggr_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_TABLE: {
			logging::debug << dynamic_cast<cofmsg_table_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_PORT: {
			logging::debug << dynamic_cast<cofmsg_port_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_QUEUE: {
			logging::debug << dynamic_cast<cofmsg_queue_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_GROUP: {
			logging::debug << dynamic_cast<cofmsg_group_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_GROUP_DESC: {
			logging::debug << dynamic_cast<cofmsg_group_desc_stats_reply const&>( msg );
		} break;
		case openflow12::OFPST_GROUP_FEATURES: {
			logging::debug << dynamic_cast<cofmsg_group_features_stats_reply const&>( msg );
		} break;
		// TODO: experimenter statistics
		default: {
			logging::debug << dynamic_cast<cofmsg_stats_reply const&>( msg );
		} break;
		}

	} break;
	case openflow12::OFPT_BARRIER_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_barrier_request const&>( msg );
	} break;
	case openflow12::OFPT_BARRIER_REPLY: {
		logging::debug << dynamic_cast<cofmsg_barrier_reply const&>( msg );
	} break;
	case openflow12::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_request const&>( msg );
	} break;
	case openflow12::OFPT_QUEUE_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_reply const&>( msg );
	} break;
	case openflow12::OFPT_ROLE_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_role_request const&>( msg );
	} break;
	case openflow12::OFPT_ROLE_REPLY: {
		logging::debug << dynamic_cast<cofmsg_role_reply const&>( msg );
	} break;
	case openflow12::OFPT_GET_ASYNC_REQUEST: {
    	logging::debug << dynamic_cast<cofmsg_get_async_config_request const&>( msg );
    } break;
	case openflow12::OFPT_GET_ASYNC_REPLY: {
		logging::debug << dynamic_cast<cofmsg_get_async_config_reply const&>( msg );
	} break;
	case openflow12::OFPT_SET_ASYNC: {
    	logging::debug << dynamic_cast<cofmsg_set_async_config const&>( msg );
    } break;
	default: {
		logging::debug << "[rofl][sock] unknown message " << msg << std::endl;
	} break;
	}
}



void
crofsock::log_of13_message(
		cofmsg const& msg)
{
	switch (msg.get_type()) {
	case openflow13::OFPT_HELLO: {
		logging::debug << dynamic_cast<cofmsg_hello const&>( msg );
	} break;
	case openflow13::OFPT_ECHO_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_echo_request const&>( msg );
	} break;
	case openflow13::OFPT_ECHO_REPLY: {
		logging::debug << dynamic_cast<cofmsg_echo_reply const&>( msg );
	} break;
	case openflow13::OFPT_EXPERIMENTER:	{
		logging::debug << dynamic_cast<cofmsg_experimenter const&>( msg );
	} break;
	case openflow13::OFPT_FEATURES_REQUEST:	{
		logging::debug << dynamic_cast<cofmsg_features_request const&>( msg );
	} break;
	case openflow13::OFPT_FEATURES_REPLY: {
		logging::debug << dynamic_cast<cofmsg_features_reply const&>( msg );
	} break;
	case openflow13::OFPT_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_get_config_request const&>( msg );
	} break;
	case openflow13::OFPT_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_get_config_reply const&>( msg );
	} break;
	case openflow13::OFPT_SET_CONFIG: {
		logging::debug << dynamic_cast<cofmsg_set_config const&>( msg );
	} break;
	case openflow13::OFPT_PACKET_OUT: {
		logging::debug << dynamic_cast<cofmsg_packet_out const&>( msg );
	} break;
	case openflow13::OFPT_PACKET_IN: {
		logging::debug << dynamic_cast<cofmsg_packet_in const&>( msg );
	} break;
	case openflow13::OFPT_FLOW_MOD: {
		logging::debug << dynamic_cast<cofmsg_flow_mod const&>( msg );
	} break;
	case openflow13::OFPT_FLOW_REMOVED: {
		logging::debug << dynamic_cast<cofmsg_flow_removed const&>( msg );
	} break;
	case openflow13::OFPT_GROUP_MOD: {
		logging::debug << dynamic_cast<cofmsg_group_mod const&>( msg );
	} break;
	case openflow13::OFPT_PORT_MOD: {
		logging::debug << dynamic_cast<cofmsg_port_mod const&>( msg );
	} break;
	case openflow13::OFPT_PORT_STATUS: {
		logging::debug << dynamic_cast<cofmsg_port_status const&>( msg );
	} break;
	case openflow13::OFPT_TABLE_MOD: {
		logging::debug << dynamic_cast<cofmsg_table_mod const&>( msg );
	} break;
	case openflow13::OFPT_MULTIPART_REQUEST: {
		cofmsg_multipart_request const& stats = dynamic_cast<cofmsg_multipart_request const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow13::OFPMP_DESC: {

		} break;
		case openflow13::OFPMP_FLOW: {

		} break;
		case openflow13::OFPMP_AGGREGATE: {

		} break;
		case openflow13::OFPMP_TABLE: {

		} break;
		case openflow13::OFPMP_PORT_DESC: {

		} break;
		case openflow13::OFPMP_QUEUE: {

		} break;
		case openflow13::OFPMP_GROUP: {

		} break;
		case openflow13::OFPMP_GROUP_DESC: {

		} break;
		case openflow13::OFPMP_GROUP_FEATURES: {

		} break;
		// TODO: experimenter statistics
		default: {

		} break;
		}

	} break;
	case openflow13::OFPT_MULTIPART_REPLY: {
		cofmsg_multipart_reply const& stats = dynamic_cast<cofmsg_multipart_reply const&>( msg );
		switch (stats.get_stats_type()) {
		case openflow13::OFPMP_DESC: {

		} break;
		case openflow13::OFPMP_FLOW: {

		} break;
		case openflow13::OFPMP_AGGREGATE: {

		} break;
		case openflow13::OFPMP_TABLE: {

		} break;
		case openflow13::OFPMP_PORT_DESC: {

		} break;
		case openflow13::OFPMP_QUEUE: {

		} break;
		case openflow13::OFPMP_GROUP: {

		} break;
		case openflow13::OFPMP_GROUP_DESC: {

		} break;
		case openflow13::OFPMP_GROUP_FEATURES: {

		} break;
		// TODO: experimenter statistics
		default: {

		} break;
		}

	} break;

	case openflow13::OFPT_BARRIER_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_barrier_request const&>( msg );
	} break;
	case openflow13::OFPT_BARRIER_REPLY: {
		logging::debug << dynamic_cast<cofmsg_barrier_reply const&>( msg );
	} break;
	case openflow13::OFPT_QUEUE_GET_CONFIG_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_request const&>( msg );
	} break;
	case openflow13::OFPT_QUEUE_GET_CONFIG_REPLY: {
		logging::debug << dynamic_cast<cofmsg_queue_get_config_reply const&>( msg );
	} break;
	case openflow13::OFPT_ROLE_REQUEST: {
		logging::debug << dynamic_cast<cofmsg_role_request const&>( msg );
	} break;
	case openflow13::OFPT_ROLE_REPLY: {
		logging::debug << dynamic_cast<cofmsg_role_reply const&>( msg );
	} break;
	case openflow13::OFPT_GET_ASYNC_REQUEST: {
    	logging::debug << dynamic_cast<cofmsg_get_async_config_request const&>( msg );
    } break;
	case openflow13::OFPT_GET_ASYNC_REPLY: {
		logging::debug << dynamic_cast<cofmsg_get_async_config_reply const&>( msg );
	} break;
	case openflow13::OFPT_SET_ASYNC: {
    	logging::debug << dynamic_cast<cofmsg_set_async_config const&>( msg );
    } break;
	default: {
		logging::debug << "[rofl][sock] unknown message " << msg << std::endl;
	} break;
	}
}
