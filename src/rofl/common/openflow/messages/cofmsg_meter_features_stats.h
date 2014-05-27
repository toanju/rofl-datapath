/*
 * cofmsg_meter_features_stats.h
 *
 *  Created on: 27.05.2014
 *      Author: andi
 */

#ifndef COFMSG_METER_FEATURES_H_
#define COFMSG_METER_FEATURES_H_ 1

#include <inttypes.h>
#include <map>

#include "rofl/common/openflow/messages/cofmsg_stats.h"
#include "rofl/common/openflow/cofmeterfeatures.h"

namespace rofl {
namespace openflow {

/**
 *
 */
class cofmsg_meter_features_stats_request :
	public cofmsg_stats_request
{
public:


	/** constructor
	 *
	 */
	cofmsg_meter_features_stats_request(
			uint8_t of_version = rofl::openflow::OFP_VERSION_UNKNOWN,
			uint32_t xid = 0,
			uint16_t stats_flags = 0);


	/**
	 *
	 */
	cofmsg_meter_features_stats_request(
			const cofmsg_meter_features_stats_request& msg);


	/**
	 *
	 */
	cofmsg_meter_features_stats_request&
	operator= (
			const cofmsg_meter_features_stats_request& msg);


	/** destructor
	 *
	 */
	virtual
	~cofmsg_meter_features_stats_request();


	/**
	 *
	 */
	cofmsg_meter_features_stats_request(
			rofl::cmemory *memarea);



public:

	friend std::ostream&
	operator<< (std::ostream& os, cofmsg_meter_features_stats_request const& msg) {
		os << dynamic_cast<cofmsg_stats_request const&>( msg );
		os << indent(2) << "<cofmsg_meter_features_request >" << std::endl;
		return os;
	};
};








/**
 *
 */
class cofmsg_meter_features_stats_reply :
	public cofmsg_stats_reply
{
public:


	/** constructor
	 *
	 */
	cofmsg_meter_features_stats_reply(
			uint8_t of_version = rofl::openflow::OFP_VERSION_UNKNOWN,
			uint32_t xid = 0,
			uint16_t stats_flags = 0,
			const rofl::openflow::cofmeter_features_reply& meter_features = rofl::openflow::cofmeter_features_reply());


	/**
	 *
	 */
	cofmsg_meter_features_stats_reply(
			cofmsg_meter_features_stats_reply const& msg);


	/**
	 *
	 */
	cofmsg_meter_features_stats_reply&
	operator= (
			cofmsg_meter_features_stats_reply const& msg);


	/** destructor
	 *
	 */
	virtual
	~cofmsg_meter_features_stats_reply();


	/**
	 *
	 */
	cofmsg_meter_features_stats_reply(cmemory *memarea);


	/** reset packet content
	 *
	 */
	virtual void
	reset();


	/**
	 *
	 */
	virtual uint8_t*
	resize(size_t len);


	/** returns length of packet in packed state
	 *
	 */
	virtual size_t
	length() const;


	/**
	 *
	 */
	virtual void
	pack(uint8_t *buf = (uint8_t*)0, size_t buflen = 0);


	/**
	 *
	 */
	virtual void
	unpack(uint8_t *buf, size_t buflen);


	/** parse packet and validate it
	 */
	virtual void
	validate();

public:

	/**
	 *
	 */
	const rofl::openflow::cofmeter_features_reply&
	get_meter_features_reply() const { return meter_features; };

	/**
	 *
	 */
	rofl::openflow::cofmeter_features_reply&
	set_meter_features_reply() { return meter_features; };


public:

	friend std::ostream&
	operator<< (std::ostream& os, cofmsg_meter_features_stats_reply const& msg) {
		os << dynamic_cast<cofmsg_stats_reply const&>( msg );
		os << indent(2) << "<cofmsg_meter_features_reply >" << std::endl;
		indent i(4); os << msg.get_meter_features_reply();
		return os;
	};

private:

	rofl::openflow::cofmeter_features_reply 	meter_features;
};

} // end of namespace openflow
} // end of namespace rofl

#endif /* COFMSG_METER_FEATURES_STATS_H_ */
