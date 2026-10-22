/*
 * Follow the sync master's channel sequence verbatim (airdrop-mt7921
 * FINDINGS §25: iOS 26 stops sending to a peer whose advertised sequence is
 * not recognisably its own). Shared by main.c and tests/awdl_sim_test.c.
 */
#ifndef AWDL_CHANSEQ_FOLLOW_H
#define AWDL_CHANSEQ_FOLLOW_H

#include <string.h>

#include "channel.h"
#include "election.h"
#include "log.h"
#include "peers.h"
#include "state.h"

/* FILL = INTERSECT, plus radio_channel bytes in the master's empty slots (HL_CHANSEQ_FILL).
 * owl's enum has no such strategy and we must not change it; the value is only ever stored in
 * awdl->channel.strategy, which schedule.c compares against ROTATE alone, so any unused value works. */
#define CHANSEQ_FILL ((enum awdl_chanseq_strategy) 100)

/* Returns 1 if the advertised sequence changed.
 * VERBATIM copies the master's sequence. INTERSECT advertises only the slots on
 * radio_channel (the rest empty), so the phone stops sending to us in slots we
 * cannot hear; with no overlap at all it declares the same as VERBATIM rather
 * than leave the phone with nothing it could ever transmit in. A changed
 * strategy rebuilds the declaration on the next call.
 * FILL starts from INTERSECT and also copies the master's own radio_channel slot bytes into the
 * slots the master leaves empty (it is off-channel or idle there), inviting it to schedule more
 * traffic on our channel. Nothing is invented (same rule as awdl_chanseq_widen). The TX gate
 * (awdl_same_channel_as_peer) needs the PEER's sequence to be on our channel in the current slot
 * too, so in a filled slot, where the master has 0 or another channel, we stay silent; we only
 * listen there. With no radio_channel slot to copy, it declares the same as VERBATIM. */
static inline int chanseq_follow_master(struct awdl_state *awdl, int radio_channel,
                                        enum awdl_chanseq_strategy strategy) {
	struct awdl_peer *src = NULL;
	if (!awdl_election_is_sync_master(&awdl->election, &awdl->self_address) &&
	    awdl_peer_get(awdl->peers.peers, &awdl->election.sync_addr, &src) >= 0 && src->has_sequence) {
		struct awdl_chan want[AWDL_CHANSEQ_LENGTH];
		int overlap = -1;
		if (strategy == AWDL_CHANSEQ_INTERSECT || strategy == CHANSEQ_FILL)
			overlap = awdl_chanseq_intersect(want, src->sequence, src->sequence_enc, (uint8_t) radio_channel);
		if (overlap <= 0)
			memcpy(want, src->sequence, sizeof(want));
		else if (strategy == CHANSEQ_FILL) {
			struct awdl_chan fill = src->sequence[0]; /* overwritten: overlap > 0 means a match exists */
			for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++) {
				if (awdl_chan_num(src->sequence[i], src->sequence_enc) == radio_channel) {
					fill = src->sequence[i];
					break;
				}
			}
			for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++) {
				if (!awdl_chan_num(src->sequence[i], src->sequence_enc))
					want[i] = fill;
			}
		}
		if (awdl->channel.sequence_from_peer && awdl->channel.strategy == strategy &&
		    awdl->channel.enc == src->sequence_enc && !memcmp(awdl->channel.sequence, want, sizeof(want)))
			return 0;
		awdl->channel.enc = src->sequence_enc;
		memcpy(awdl->channel.sequence, want, sizeof(want));
		awdl->channel.strategy = strategy;
		awdl->channel.sequence_from_peer = 1;
		awdl->channel.seq_src = src->addr;
		int on = 0;
		for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++)
			on += awdl_chan_num(awdl->channel.sequence[i], awdl->channel.enc) == radio_channel;
		const char *name = strategy == CHANSEQ_FILL             ? "fill"
		                   : strategy == AWDL_CHANSEQ_INTERSECT ? "intersect"
		                                                        : "verbatim";
		int narrowed = strategy == AWDL_CHANSEQ_INTERSECT || strategy == CHANSEQ_FILL;
		log_info("following %s (%s) [%s%s]: %d/16 slots on ch %d", ether_ntoa(&src->addr), src->name, name,
		         narrowed && overlap <= 0 ? ", no overlap, verbatim declared" : "", on, radio_channel);
		return 1;
	}
	if (awdl->channel.sequence_from_peer) {
		log_info("no sync master, back to static channel sequence");
		awdl_chanseq_init_static(awdl->channel.sequence, &awdl->channel.master);
		awdl->channel.enc = AWDL_CHAN_ENC_OPCLASS;
		awdl->channel.sequence_from_peer = 0;
		return 1;
	}
	return 0;
}

#endif /* AWDL_CHANSEQ_FOLLOW_H */
