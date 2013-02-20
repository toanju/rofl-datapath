/*
 * platform_hooks_of12.h
 *
 *  Created on: Feb 7, 2013
 *      Author: tobi,msune
 */

#ifndef PLATFORM_HOOKS_OF12_H_
#define PLATFORM_HOOKS_OF12_H_

#include "rofl.h"
#include "../../../openflow/of_switch.h"
#include "../../../openflow/openflow12/of12_switch.h"
#include "../../../../pipeline/common/datapacket.h"

//C++ extern C
ROFL_PIPELINE_BEGIN_DECLS

/*
* Hooks for configuration
*/
//TODO: evaluate if they must return errors
rofl_result_t platform_post_init_of12_switch(of12_switch_t* sw);
rofl_result_t platform_pre_destroy_of12_switch(of12_switch_t* sw);

//Packet in
void platform_of12_packet_in(const of_switch_t* sw, uint8_t table_id, datapacket_t* pkt, of_packet_in_reason_t reason);

//Extern C
ROFL_PIPELINE_END_DECLS

#endif /* PLATFORM_HOOKS_OF12_H_ */