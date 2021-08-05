/* -*- C -*- */
/*
 * Copyright (c) 2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#pragma once

#ifndef __MOTR_DTM0_RECOVERY_H__
#define __MOTR_DTM0_RECOVERY_H__

/*
 * DTM0 basic recovery DLD
 *
 * Definition
 * ----------
 *
 *   DTM0 basic recovery defines a set of states and transitions between them
 * for each participant of the cluster considering only a limited set of
 * usecases.
 *
 *
 * Assumptions (limitations)
 * ---------------------------
 *
 *  A1: HA (Cortx-Hare or Cortx-Hare+Cortx-HA) provides EOS for each event.
 *  A2: HA provides total ordering for all events in the system.
 *  *A3: DTM0 handles HA events one-by-one without filtering.
 *  A4: DTM0 user relies on EXECUTED and STABLE events to ensure proper ordering
 *      of DTXes.
 *  A5: There may be a point on the timeline where data is not fully replicated
 *      across the cluster.
 *  A6: RPC connections to a FAILED/TRANSIENT participant are terminated as soon
 *      as HA sends the corresponding event.
 *  A7: HA epochs are not exposed to DTM0 even if HA replies on them.
 *
 *
 * HA states and HA events
 * -----------------------
 *
 *   Every participant (PA) has the following set of HA states:
 *     TRANSIENT
 *     ONLINE
 *     RECOVERING
 *     FAILED
 * States are assigned by HA: it delivers an HA event to every participant
 * whenever it decided that one of the participants has changed its state.
 *   Motr-HA subsystem provides the "receiver-side" guarantee to comply with A1.
 * It has its own log of events that is being updated whenever an HA event
 * has been fully handled.
 *
 *
 * State transitions (HA side)
 * ---------------------------
 *
 * PROCESS_STARTING: TRANSIENT -> RECOVERING
 *   HA sends "Recovering" when the corresponding
 *   participant sends PROCESS_STARTED event to HA.
 *
 * PROCESS_STARTED: RECOVERING -> ONLINE
 *   When the process finished recovering from a failure, it sends
 *   PROCESS_STARTED to HA. HA notifies everyone that this process is back
 *   ONLINE.
 *
 * <PROCESS_T_FAILED>: ONLINE -> TRANSIENT
 *   When HA detects a transient failure, it notifies everyone about it.
 *
 * <PROCESS_FAILED>: [ONLINE, RECOVERING, TRANSIENT] -> FAILED
 *   Whenever HA detects a permanent failure, it starts eviction of the
 *   corresponding participant from the cluster.
 *
 *
 * HA Events (Motr side)
 * ---------------------
 *
 *   PROCESS_STARTING is sent when Motr process is ready to start recovering
 * procedures, and it is ready to serve WRITE-like requests.
 *   PROCESS_STARTED is sent when Motr process finished its recovery procedures,
 * and it is ready to serve READ-like requests.
 *
 *
 * State transitions (DTM0 side)
 * -----------------------------
 *
 *  TRANSIENT -> RECOVERING: DTM0 service launches a recovery FOM. There are
 *    two types of recovery FOMs: local recovery FOM and remote recovery FOM.
 *    The type is based on the participant that has changed its state.
 *  RECOVERING -> ONLINE: DTM0 service stops/finalises recovery FOMs for the
 *    participant that has changed its state.
 *  ONLINE -> TRANSIENT: DTM0 service terminates DTM0 RPC links established
 *    to the participant that has changed its state (see A6).
 *  [ONLINE, RECOVERING, TRANSIENT] -> FAILED: DTM0 service launches a recovery
 *    FOM to handle eviction (remote eviction FOM).
 *
 *
 * DTM0 recovery FOMs
 * ------------------
 *
 *   Local recovery FOM. A local recovery FOM is supposed to handle recovery
 * of the process where the fom has been launched. The FOM sends
 * PROCESS_STARTING event to HA to indicate its readiness to receive REDOs.
 * When the recovery stop condition is satisfied, the FOM causes
 * PROCESS_STARTED event.
 *   Remote recovery FOM. This FOM uses the local DTM0 log to replay DTM0
 * transactions to a remote participant. The FOM launched and stopped based
 * on the HA events.
 *   Remote eviction FOM. The FOM replays every log record to every participant
 * (where this log record is not persistent) if the FAILED participant belongs
 * the list of participants of the corresponding transaction descriptor
 * (or if it is the originator of the transaction). Note, eviction happens
 * without any state transitions of the other participants.
 *
 * DTM0 recovery stop condition
 * ----------------------------
 *   The stop condition is used to stop recovery FOMs and propagate state
 * transitions to HA. Recovery stops when a participant "catches up" with
 * the new cluster state. Since we allow WRITE-like operations to be performed
 * at almost any time (the exceptions are TRANSIENT and FAILED states of
 * of the target participants), the stop condition relies on the information
 * about the difference between the DTM0 log state when recovery started (1)
 * and the point where recovery ended (2). The second point is defined using
 * so-called "recovery stop condition". Each recovery FOM has its own condition
 * described in the next sections.
 *   Note, HA epochs would let us define a quite precise boundaries for the
 * stop. However, as it is stated in A7, we cannot rely on them. Because of
 * that, we simply store the first WRITE-like operation on the participant
 * experiencing RECOVERING, so that this participant can make a decision
 * whether recovery has to be stopped or not.
 *
 *
 * Stop condition for local FOM
 * ----------------------------
 *
 *   The local recovery FOM is created before the local process sends
 * PROCESS_STARTED event the HA. Then, it stores (in volatile memory) the first
 * WRITE-like operation that arrived after the FOM was created. Since clients
 * cannot send WRITE-like operations to servers in TRANSIENT or FAILED state,
 * and leaving of such a state happens only a local recovery FOM is created,
 * it helps to avoid a race window, as it is shown below:
 *
 * @verbatim
 *   P1.HA     | P1.TRANSIENT        | P1.RECOVERING
 *   P1.Motr   | <down> | <starting> |
 *   P1.DTM    | <down>                          | <started recovery FOM>
 *   C1.HA         | P1.TRANSIENT          | P1.RECOVERING
 *   C1.Motr   | <ongoing IO to other PAs>  | <sends KVS PUT to P1>
 *             -------------------------------------------------> real time
 *                                   1    2 3    4
 *
 *   (1) HA on P1 learns about the state transition.
 *   (2) HA on P2 learns about the state transition.
 *   (3) Client sends KVS PUT (let's say it gets delivered almost instantly).
 *   (4) A recovery FOM was launched on P1. It missed the KVS PUT.
 * @endverbatim
 *
 *   The stop condition for a local recovery FOM can be broken down to the
 * following sub-conditions applied to every ONLINE-or-TRANSIENT participant
 * of the cluster:
 *   1. Caught-up: participant sends a log record that has the same TX ID as
 *   the first operation recorded by the local recovery FOM.
 *   2. End of log: participant send "end-of-the-log" message.
 * In other words, a participant leaves RECOVERING state when it got REDOs
 * from all the others until the point where new IO was started. Note, there
 * are a lot of corner cases related to interleaving and clock synchronisation
 * but they are not covered here.
 *
 *
 * Stop condition for remote FOM
 * -----------------------------
 *
 *   The next question is "when a remote process should stop sending REDOs?".
 * This question defines the stop condition for a remote recovery FOM. Since
 * there is a race window between the point where a remote process gets
 * RECOVERING event and the point where one of the clients sends a "new"
 * WRITE-like operations, remote participants are not able to use the
 * "until first WRITE" condition. Instead, they rely on HA state transitions.
 * When a participant leaves RECOVERING state, all remote recovery FOMs should
 * be stopped.
 *   When a remote recovery FOM reaches the end of the local DTM0 log, it
 * goes to sleep. It gets awaken when a new record appears in the log. As it
 * was mentioned above, this kind of FOM can be stopped only when HA notifies
 * about state transition of the participant that is being recovered.
 *
 * Stop condition for eviction FOM
 * -------------------------------
 *
 *   The final question is how to stop eviction of a FAILED participant. The
 * stop condition here is that all the records where the participant
 * participated should be replayed to the other participants.
 *   An eviction FOM stops after all the needed log records were replayed.
 *
 *
 * REDO message
 * ------------
 *
 *   REDO messages contain full updates (as opposite to PERSISTENT messages
 * that carry only partial updates). They are sent one-by-one by a
 * recover FOM (remote/eviction). A REDO message is populated using
 * a DTM0 log record. User-specific service provides necessary callbacks
 * to transmute the message if it is required.
 *   A log record can be removed only by the local log pruner daemon.
 * DTM0 log locks or the locality lock can be used to avoid such kind of
 * conflicts (up to the point where a shot-lived lock can be taken to make a
 * copy of a log record).
 *   The REDO message is just a wrapper around user-specific WRITE-like
 * operation (for example, CAS op). Aside from user request, it may contain
 * the most recent information about the states of the transaction on the
 * other participants and information that helps to identify the sender
 * DTM0 service:
 * @verbatim
 *   REDO message request {
 *      struct pa_state latest_states[];
 *      uint64_t        source;
 *      struct buf      original_request;
 *   }
 *   REDO message reply {
 *      struct buf      reply_to_original_request;
 *   }
 * @endverbatim
 *   The receiver should send back a packed reply or an empty reply. The reply
 * serves two purposes:
 *   - Establishes back-pressure. As it was described above, a remote recovery
 *   or eviction FOM sends packets one-by-one awaiting replies each time. This
 *   could be a subject to some batching policy in future.
 *   - Helps to detect failures. For example, if one of the participants
 *   reports an error (ENOMEM or ENOENT) then it may mean that HA should
 *   put the participant into TRANSIENT state (by rebooting the OS process)
 *   or even mark it as failed (for example, if a certain index does
 *   not exist there).
 *
 *
 * REDO FOM
 * --------
 *
 *   Whenever a REDO message is received, DTM0 service schedules a REDO FOM.
 * The FOM creates a user-specific FOP (for example, CAS FOP), then it creates
 * an FOM for this FOP (let's call it uFOM). DTM0 service uses fom_interpose
 * API to execute the uFOM. Upon uFOM completion, the REDO FOM sends back
 * an RPC reply with REDO message reply FOP.
 *   Additional information carried by a REDO fop may be applied separately
 * to the DTM0 log.
 *
 *
 * Recovery and HA EOS
 * -------------------
 *
 *   HA EOS is needed to replay the events sent to the local participant
 * in order to "catch up" with the most recent state of the cluster.
 * If some the events were "consumed" (committed) by DTM0 then
 * they should not be replayed.
 *   Here is a list of allowed state transitions for a participant:
 *     TRANSIENT  -> RECOVERING (1)
 *     TRANSIENT  -> FAILED     (2)
 *     RECOVERING -> ONLINE     (3)
 *     RECOVERING -> FAILED     (4)
 *     RECOVERING -> TRANSIENT  (5)
 *     ONLINE     -> TRANSIENT  (6)
 *     ONLINE     -> FAILED     (7)
 *     FAILED     -> EVICTED    (*)
 *   Note that replay if the local HA log may be required only if
 * the participant enters TRANSIENT state: all other transitions happen
 * either without losing of the volatile state of the participant or
 * with losing of its permanent state (which means its volatile state
 * cannot be recovered at all).
 *   When a participant enters TRANSIENT (5 and 6), its previous transitions
 * (1, 3, 4, 5) get "consumed".
 *   When a participant leaves RECOVERING (3, 4, 5), its previous transition
 * (1) gets "consumed".
 *   When a participant enters FAILED, its previous transitions (1, 3, 5, 6)
 *  get "consumed".
 *   When a participant leaves FAILED (*), its previous transition (7) gets
 * "consumed". Note, this transition is not well-defined yet. If the transition
 * (7) causes every participant enter RECOVERING (i.e, ONLINE -> RECOVERING
 * is allowed) then this is not required, so that any transition to FAILED
 * gets "consumed" as soon as it gets delivered.
 *
 *
 * Originators' reaction to TRANSIENT failures
 * -------------------------------------
 *
 *  When one of the participants with persistent state enters TRANSIENT,
 * an originator (a participant without persistent state) must force
 * the DTXes that contain this TRANSIENT participant to progress without
 * waiting on a reply ("EXECUTED") or on a DTM0 PERSISTENT message.
 *  Originator's DTM0 service participates in recovery process in the
 * same way as the other types of participants.
 */

#endif /* __MOTR_DTM0_RECOVERY_H__ */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
