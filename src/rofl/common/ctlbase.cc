/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ctlbase.h"

ctlbase::ctlbase(
		uint64_t lldpid,
		std::string dpname,
		uint64_t dpid,
		uint8_t n_tables,
		uint32_t n_buffers,
		caddress const& rpc_ctl_addr,
		caddress const& rpc_dpt_addr) :
	cfwdelem(dpname, dpid, n_tables, n_buffers, rpc_ctl_addr, rpc_dpt_addr),
	lldpid(lldpid),
	dpath(0)
{

}


ctlbase::~ctlbase()
{
	while (not adapters.empty())
	{
		delete (*(adapters.begin()));
	}

	std::map<cofbase*, cofdpath*>::iterator it;
	for (it = ofdpath_list.begin(); it != ofdpath_list.end(); ++it)
	{
		cofmatch match; // all wildcard
		it->second->fsp_close(match);
	}
}


const char*
ctlbase::c_str()
{
	cvastring vas(2048);

	info.assign(vas("\nctlbase(%s): =>", dpname.c_str()));

#if 0
	std::set<cadapt*>::iterator it;
	info.append(vas("\nlist of registered cctlmod instances: =>"));
	for (it = adapters.begin(); it != adapters.end(); ++it)
	{
		//info.append(vas("\n  %s", (*it)->c_str()));
	}
#endif

	//info.append(vas("\n%s", cfwdelem::c_str()));

	return info.c_str();
}


void
ctlbase::handle_dpath_open(
		cofdpath *dpath)
{
	if ((0 == dpath) || (dpath->dpid != lldpid))
	{
		return;
	}

	this->dpath = dpath;

	/*
	 * inform adapters about existence of our layer (n-1) datapath
	 */
	for (std::set<cadapt*>::iterator
			it = adapters.begin(); it != adapters.end(); ++it)
	{
		(*it)->handle_dpath_open(dpath);
	}
}


void
ctlbase::handle_dpath_close(
		cofdpath *dpath)
{
	if ((0 == dpath) || (dpath->dpid != lldpid))
	{
		return;
	}

	/*
	 * inform adapters about detachment of our layer (n-1) datapath
	 */
	for (std::set<cadapt*>::iterator
			it = adapters.begin(); it != adapters.end(); ++it)
	{
		(*it)->handle_dpath_close(dpath);
	}

	this->dpath = (cofdpath*)0;
}


void
ctlbase::handle_stats_reply(
		cofdpath *sw,
		cofpacket *pack)
{

}


void
ctlbase::handle_error(
		cofdpath *sw,
		cofpacket *pack)
{

}



void
ctlbase::flowspace_open(
		cadapt *adapt,
		cofmatch const& m) throw (eCtlBaseNotConnected)
{
	if (0 == dpath)
	{
		throw eCtlBaseNotConnected();
	}

	try {
		dpath->fsptable.insert_fsp_entry(adapt, m, false /*non-strict*/);

		cofmatch match(m);
		WRITELOG(CFWD, DBG, "ctlbase(%s)::flowspace_open() rcvd cofmatch from adapter: %s:\n%s",
				dpname.c_str(), adapt->c_str(), match.c_str());

		dpath->fsp_open(m);

	} catch (eOFbaseNotAttached& e) {

	}
}


void
ctlbase::flowspace_close(
		cadapt *adapt,
		const cofmatch & m)
{
	try {

		dpath->fsptable.delete_fsp_entry(adapt, m, false /*non-strict*/);

		cofmatch match(m);
		WRITELOG(CFWD, DBG, "ctlbase(%s)::down_fsp_close() rcvd cofmatch from adapter: %s:\n%s",
				dpname.c_str(), adapt->c_str(), match.c_str());

		dpath->fsp_close(m);

	} catch (eOFbaseNotAttached& e) {

	}
}






uint32_t
ctlbase::get_free_portno()
throw (eAdaptNotFound)
{
	uint32_t portno = 1;
	while (n_ports.find(portno) != n_ports.end())
	{
		portno++;
		if (portno == std::numeric_limits<uint32_t>::max())
		{
			throw eAdaptNotFound();
		}
	}
	return portno;
}







/*
 * PACKET-IN
 */



/*
 * received by layer (n-1) datapath, sending to adapters for doing the adaptation
 */
void
ctlbase::handle_packet_in(
		cofdpath *sw,
		cofpacket *pack)
{
	WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() pack:%s",
			dpname.c_str(), pack->c_str());

	if (sw != dpath)
	{
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() rcvd packet from non-registered dpath",
				dpname.c_str());

		delete pack; return;
	}

	try {
		std::set<cfspentry*> fsp_list =
				sw->fsptable.find_matching_entries(
						pack->match.get_in_port(),
						be16toh(pack->ofh_packet_in->total_len),
						(uint8_t*)pack->get_data(), pack->get_datalen());

		if (fsp_list.size() > 1)
		{
			throw eCtlBaseInval();
		}

		cfspentry* fspentry = (*(fsp_list.begin()));

		cadapt *adapt = dynamic_cast<cadapt*>( fspentry->fspowner );
		if (0 == adapt)
		{
			throw eInternalError();
		}

#if 1
		cpacket tmppack(
					pack->get_data(),
					pack->get_datalen(),
					pack->match.get_in_port(), false /* = do not classify */);

		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"flowspace subscription for packet found, data: %s\nctlmod: %s",
				dpname.c_str(), tmppack.c_str(), adapt->c_str());
#endif

		adapt->handle_packet_in(pack);



	} catch (eFspNoMatch& e) {

		cpacket tmppack(
					pack->get_data(),
					pack->get_datalen(),
					pack->match.get_in_port());

		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"no flowspace subscription for packet found, data: %s",
				dpname.c_str(), tmppack.c_str());
		delete pack;

	} catch (eCtlBaseInval& e) {
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"too many subscriptions found for packet, unspecified behaviour, pack: %s",
				dpname.c_str(), pack->c_str());
		delete pack;

	} catch (eInternalError& e) {
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"internal error, found fspentry owner which is not of type cctlmod. WTF???, pack: %s",
				dpname.c_str(), pack->c_str());
		delete pack;

	} catch (eFrameInvalidSyntax& e) {
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"frame with invalid syntax received, dropping. pack: %s",
				dpname.c_str(), pack->c_str());
		delete pack;

	} catch (eOFpacketNoData& e) {
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_packet_in() "
				"PACKET-IN without attached payload, dropping. pack: %s",
				dpname.c_str(), pack->c_str());
				delete pack;
	}
}


/*
 * received from adapter, sending to derived transport controller
 */
void
ctlbase::send_packet_in(
		cadapt *adapt,
		uint32_t buffer_id,
		uint16_t total_len,
		uint8_t table_id,
		uint8_t reason,
		cofmatch& match,
		fframe& frame)
{
	handle_packet_in(
			buffer_id,
			total_len,
			table_id,
			reason,
			match,
			frame); // let derived transport controller handle incoming packet
}












/*
 * PORT-STATUS
 */

/*
 * received by layer (n-1) datapath, sending to adapters for handling changes in port status
 */
void
ctlbase::handle_port_status(
		cofdpath *sw,
		cofpacket *pack,
		cofport *port)
{
	WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_port_status() %s", s_dpid.c_str(), cfwdelem::c_str());

	if (sw != dpath)
	{
		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_port_status() rcvd packet from non-registered dpath",
				dpname.c_str());

		delete pack; return;
	}

	std::set<cadapt*>::iterator it;
	for (it = adapters.begin(); it != adapters.end(); ++it)
	{
		(*it)->handle_port_status(
						pack->ofh_port_status->reason,
						port);
	}

	delete pack;
}


/*
 * received from adapter, sending to derived transport controller
 */
void
ctlbase::send_port_status(
				cadapt *adapt,
				uint8_t reason,
				cofport *ofport)
{
	if ((0 == adapt) || (0 == ofport))
	{
		return;
	}

	if (n_ports.find(ofport->port_no) == n_ports.end())
	{
		return;
	}

	if (n_ports[ofport->port_no] != adapt)
	{
		return;
	}

	WRITELOG(CFWD, DBG, "ctlbase(%p)::send_port_status() %s", this, c_str());

	switch (reason) {
	case OFPPR_ADD:
		{
			/* sanity check: the port_no must not be in use currently */
			if (n_ports.find(ofport->port_no) != n_ports.end())
			{
				throw eCtlBaseExists();
			}

			n_ports[ofport->port_no] = adapt;
		}
		break;
	case OFPPR_DELETE:
		{
			/* sanity check: the port_no must exist */
			if (n_ports.find(ofport->port_no) == n_ports.end())
			{
				return;
			}
			n_ports.erase(ofport->port_no);
		}
		break;
	case OFPPR_MODIFY:
		{
			/* sanity check: the port_no must exist */
			if (n_ports.find(ofport->port_no) == n_ports.end())
			{
				throw eCtlBaseNotFound();
			}
		}
		break;
	default:

		break;
	}

	handle_port_status(reason, ofport); // call method from derived transport controller
}











/*
 * PACKET-OUT
 */


/*
 * received from derived transport controller
 * replacing cfwdelem's version of send_packet_out_message()
 */
void
ctlbase::send_packet_out_message(
		uint32_t buffer_id,
		uint32_t in_port,
		cofaclist const& aclist,
		cpacket *pack) throw (eCtlBaseInval)
{
	if (0 == dpath)
	{
		return;
	}

	/*
	 * iterate over all actions in aclist and call adapt methods
	 */
	cofaclist actions; // list of adapted actions

	for (cofaclist::const_iterator
			it = aclist.begin(); it != aclist.end(); ++it)
	{
		cofaction action(*it);

		switch (action.get_type()) {
		case OFPAT_OUTPUT:
			{
				uint32_t out_port = be32toh(action.oac_output->port);

				if (n_ports.find(out_port) == n_ports.end())
				{
					throw eCtlBaseInval(); // outgoing port is invalid
				}

				n_ports[out_port]->filter_action_list(actions);

			}
			break;
		default:
			{
				actions.next() = action; // push other actions directly on the list of adapted actions
			}
			break;
		}
	}

	cmemory mem(pack->length());

	pack->pack(mem.somem(), mem.memlen());

	cfwdelem::send_packet_out_message(
						dpath,
						buffer_id,
						in_port,
						actions,
						mem.somem(), mem.memlen());
}


/*
 * received from adapter
 */
void
ctlbase::send_packet_out(
				cadapt *adapt,
				uint32_t buffer_id,
				uint32_t in_port,
				cofaclist& actions,
				cpacket *pack)
{
	if (0 == dpath)
	{
		return;
	}

	cmemory mem(pack->length());

	pack->pack(mem.somem(), mem.memlen());

	cfwdelem::send_packet_out_message(
						dpath,
						buffer_id,
						in_port,
						actions,
						mem.somem(), mem.memlen());
}









/*
 * FLOW-MOD
 */


/*
 * received from derived transport controller
 */
void
ctlbase::send_flow_mod_message(
			cflowentry& flowentry)
{
	send_flow_mod_message(
			flowentry.match,
			flowentry.get_cookie(),
			flowentry.get_cookie_mask(),
			flowentry.get_table_id(),
			flowentry.get_command(),
			flowentry.get_idle_timeout(),
			flowentry.get_hard_timeout(),
			flowentry.get_priority(),
			flowentry.get_buffer_id(),
			flowentry.get_out_port(),
			flowentry.get_out_group(),
			flowentry.get_flags(),
			flowentry.instructions);
}


/*
 * received from derived transport controller
 */
void
ctlbase::send_flow_mod_message(
		cofmatch& ofmatch,
		uint64_t cookie,
		uint64_t cookie_mask,
		uint8_t table_id,
		uint8_t command,
		uint16_t idle_timeout,
		uint16_t hard_timeout,
		uint16_t priority,
		uint32_t buffer_id,
		uint32_t out_port,
		uint32_t out_group,
		uint16_t flags,
		cofinlist const& inlist)
{
	try {
		if (0 == dpath)
		{
			return;
		}

		/*
		 * adjust match on incoming port only
		 */
		cofmatch match(ofmatch); // new matches

		uint32_t in_port = match.get_in_port();

		if ((OFPP_CONTROLLER != in_port) && (n_ports.find(in_port) == n_ports.end()))
		{
			throw eCtlBaseInval();
		}

		n_ports[in_port]->filter_match(match);





		/*
		 * iterate over all instructions and find affected adapters by their port-no
		 */
		cofinlist insts; // new instructions

		for (cofinlist::const_iterator
				it = inlist.begin(); it != inlist.end(); ++it)
		{
			cofinst inst(*it);

			switch (inst.get_type()) {
			case OFPIT_APPLY_ACTIONS:
			case OFPIT_WRITE_ACTIONS:
				{
					for (std::set<cadapt*>::iterator
							it = adapters.begin(); it != adapters.end(); ++it)
					{
						cadapt* adapt = (*it);

						adapt->filter_action_list(inst.actions);
					}
				}
				break;
			}

			insts.next() = inst;
		}

		WRITELOG(CFWD, DBG, "ctlbase(%s)::send_flow_mod_message() %s",
					   dpname.c_str(), insts.c_str());


		cfwdelem::send_flow_mod_message(
							dpath,
							match,
							cookie,
							cookie_mask,
							table_id,
							command,
							idle_timeout,
							hard_timeout,
							priority,
							buffer_id,
							out_port,
							out_group,
							flags,
							insts);


	} catch (eOFmatchNotFound& e) {

		WRITELOG(CFWD, DBG, "ctlbase(%s)::handle_flow_mod() "
					   "no in_port found in Flow-Mod message, ignoring",
					   dpname.c_str());

		throw eCtlBaseInval();

	}
}









void
ctlbase::send_stats_request(
		uint16_t type,
		uint16_t flags,
		uint8_t *body,
		size_t bodylen)

{
	if (0 == dpath)
	{
		return;
	}

	switch (type) {
	case OFPST_DESC:
	case OFPST_TABLE:
	case OFPST_GROUP:
	case OFPST_GROUP_DESC:
	case OFPST_GROUP_FEATURES:
	case OFPST_EXPERIMENTER:
		{
			cfwdelem::send_stats_request(dpath, type, flags, body, bodylen);
		}
		break;
	case OFPST_FLOW:
		{
			throw eNotImplemented();
		}
		break;
	case OFPST_AGGREGATE:
		{
			throw eNotImplemented();
		}
		break;
	case OFPST_PORT:
		{
			throw eNotImplemented();
		}
		break;
	case OFPST_QUEUE:
		{
			throw eNotImplemented();
		}
		break;
	default:
		{

		}
		break;
	}
}


void
ctlbase::send_group_mod_message(
			cgroupentry& groupentry)
{
	if (0 == dpath)
	{
		return;
	}

	cofbclist buckets; // new list of adapted buckets

	for (cofbclist::const_iterator
			it = groupentry.buckets.begin(); it != groupentry.buckets.end(); ++it)
	{
		cofbucket bucket = (*it);

		/*
		 * iterate over all buckets and call filter_action_list
		 */
		for (std::set<cadapt*>::iterator
				it = adapters.begin(); it != adapters.end(); ++it)
		{
			cadapt* adapt = (*it);

			adapt->filter_action_list(bucket.actions);
		}

		buckets.next() = bucket;
	}

	groupentry.buckets = buckets; // store new list of adapted buckets in groupentry

	WRITELOG(CFWD, DBG, "ctlbase(%s)::send_group_mod_message() %s",
				   dpname.c_str(), groupentry.c_str());

	cfwdelem::send_group_mod_message(
								dpath,
								groupentry);
}


void
ctlbase::send_table_mod_message(
		uint8_t table_id,
		uint32_t config)
{
	if (0 == dpath)
	{
		return;
	}

	cfwdelem::send_table_mod_message(
								dpath,
								table_id,
								config);
}


void
ctlbase::send_port_mod_message(
		uint32_t port_no,
		cmacaddr const& hwaddr,
		uint32_t config,
		uint32_t mask,
		uint32_t advertise) throw (eCtlBaseNotFound)
{
	if (0 == dpath)
	{
		return;
	}

	if (n_ports.find(port_no) == n_ports.end())
	{
		throw eCtlBaseNotFound();
	}

	n_ports[port_no]->handle_port_mod(
							port_no,
							config,
							mask,
							advertise);

	/*
	 * TODO: thinking :) =>
	 * What is happening, if the transport controller tears down
	 * a port? This may affect adapted ports only or also physical ports
	 * from layer (n-1). When is this happening? Who is sending the
	 * Port-Mod message to the layer (n-1) datapath?
	 */
}







cadapt*
ctlbase::find_adaptor_by_portno(
		uint32_t portno) throw (eCtlBaseNotFound)
{
	if (n_ports.find(portno) == n_ports.end())
	{
		throw eCtlBaseNotFound();
	}

	return n_ports[portno];
}


