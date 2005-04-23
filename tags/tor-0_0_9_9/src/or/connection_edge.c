/* Copyright 2001 Matej Pfajfar.
 * Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char connection_edge_c_id[] = "$Id$";

/**
 * \file connection_edge.c
 * \brief Handle edge streams.
 **/

#include "or.h"
#include "tree.h"

static addr_policy_t *socks_policy = NULL;
/* List of exit_redirect_t */
static smartlist_t *redirect_exit_list = NULL;

static int connection_ap_handshake_process_socks(connection_t *conn);

/** There was an EOF. Send an end and mark the connection for close.
 */
int connection_edge_reached_eof(connection_t *conn) {
#ifdef HALF_OPEN
  /* eof reached; we're done reading, but we might want to write more. */
  conn->done_receiving = 1;
  shutdown(conn->s, 0); /* XXX check return, refactor NM */
  if (conn->done_sending) {
    connection_edge_end(conn, END_STREAM_REASON_DONE, conn->cpath_layer);
    connection_mark_for_close(conn);
  } else {
    connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_END,
                                 NULL, 0, conn->cpath_layer);
  }
  return 0;
#else
  if (buf_datalen(conn->inbuf) && connection_state_is_open(conn)) {
    /* it still has stuff to process. don't let it die yet. */
    return 0;
  }
  log_fn(LOG_INFO,"conn (fd %d) reached eof (stream size %d). Closing.", conn->s, (int)conn->stream_size);
  if (!conn->marked_for_close) {
    /* only mark it if not already marked. it's possible to
     * get the 'end' right around when the client hangs up on us. */
    connection_edge_end(conn, END_STREAM_REASON_DONE, conn->cpath_layer);
    connection_mark_for_close(conn);
//    conn->hold_open_until_flushed = 1; /* just because we shouldn't read
//                                          doesn't mean we shouldn't write */
  }
  return 0;
#endif
}

/** Handle new bytes on conn->inbuf based on state:
 *   - If it's waiting for socks info, try to read another step of the
 *     socks handshake out of conn->inbuf.
 *   - If it's open, then package more relay cells from the stream.
 *   - Else, leave the bytes on inbuf alone for now.
 *
 * Mark and return -1 if there was an unexpected error with the conn,
 * else return 0.
 */
int connection_edge_process_inbuf(connection_t *conn, int package_partial) {

  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  switch (conn->state) {
    case AP_CONN_STATE_SOCKS_WAIT:
      if (connection_ap_handshake_process_socks(conn) < 0) {
        conn->has_sent_end = 1; /* no circ yet */
        connection_mark_for_close(conn);
        conn->hold_open_until_flushed = 1; /* redundant but shouldn't hurt */
        return -1;
      }
      return 0;
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      if (connection_edge_package_raw_inbuf(conn, package_partial) < 0) {
        connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer);
        connection_mark_for_close(conn);
        return -1;
      }
      return 0;
    case EXIT_CONN_STATE_CONNECTING:
    case AP_CONN_STATE_RENDDESC_WAIT:
    case AP_CONN_STATE_CIRCUIT_WAIT:
    case AP_CONN_STATE_CONNECT_WAIT:
    case AP_CONN_STATE_RESOLVE_WAIT:
      log_fn(LOG_INFO,"data from edge while in '%s' state. Leaving it on buffer.",
                      conn_state_to_string[conn->type][conn->state]);
      return 0;
  }
  log_fn(LOG_WARN,"Bug: Got unexpected state %d. Closing.",conn->state);
  connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer);
  connection_mark_for_close(conn);
  return -1;
}

/** This edge needs to be closed, because its circuit has closed.
 * Mark it for close and return 0.
 */
int connection_edge_destroy(uint16_t circ_id, connection_t *conn) {
  tor_assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  if (conn->marked_for_close)
    return 0; /* already marked; probably got an 'end' */
  log_fn(LOG_INFO,"CircID %d: At an edge. Marking connection for close.",
         circ_id);
  conn->has_sent_end = 1; /* we're closing the circuit, nothing to send to */
  connection_mark_for_close(conn);
  conn->hold_open_until_flushed = 1;
  conn->cpath_layer = NULL;
  return 0;
}

/** Send a relay end cell from stream <b>conn</b> to conn's circuit,
 * with a destination of cpath_layer. (If cpath_layer is NULL, the
 * destination is the circuit's origin.) Mark the relay end cell as
 * closing because of <b>reason</b>.
 *
 * Return -1 if this function has already been called on this conn,
 * else return 0.
 */
int
connection_edge_end(connection_t *conn, char reason, crypt_path_t *cpath_layer)
{
  char payload[5];
  size_t payload_len=1;
  circuit_t *circ;

  if (conn->has_sent_end) {
    log_fn(LOG_WARN,"Harmless bug: Calling connection_edge_end (reason %d) on an already ended stream?", reason);
    return -1;
  }

  payload[0] = reason;
  if (reason == END_STREAM_REASON_EXITPOLICY) {
    /* this is safe even for rend circs, because they never fail
     * because of exitpolicy */
    set_uint32(payload+1, htonl(conn->addr));
    payload_len += 4;
  }

  circ = circuit_get_by_conn(conn);
  if (circ && !circ->marked_for_close) {
    log_fn(LOG_DEBUG,"Marking conn (fd %d) and sending end.",conn->s);
    connection_edge_send_command(conn, circ, RELAY_COMMAND_END,
                                 payload, payload_len, cpath_layer);
  } else {
    log_fn(LOG_DEBUG,"Marking conn (fd %d); no circ to send end.",conn->s);
  }

  conn->has_sent_end = 1;
  return 0;
}

/** Connection <b>conn</b> has finished writing and has no bytes left on
 * its outbuf.
 *
 * If it's in state 'open', stop writing, consider responding with a
 * sendme, and return.
 * Otherwise, stop writing and return.
 *
 * If <b>conn</b> is broken, mark it for close and return -1, else
 * return 0.
 */
int connection_edge_finished_flushing(connection_t *conn) {
  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  switch (conn->state) {
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      connection_stop_writing(conn);
      connection_edge_consider_sending_sendme(conn);
      return 0;
    case AP_CONN_STATE_SOCKS_WAIT:
    case AP_CONN_STATE_RENDDESC_WAIT:
    case AP_CONN_STATE_CIRCUIT_WAIT:
    case AP_CONN_STATE_CONNECT_WAIT:
      connection_stop_writing(conn);
      return 0;
    default:
      log_fn(LOG_WARN,"BUG: called in unexpected state %d.", conn->state);
      return -1;
  }
  return 0;
}

/** Connected handler for exit connections: start writing pending
 * data, deliver 'CONNECTED' relay cells as appropriate, and check
 * any pending data that may have been received. */
int connection_edge_finished_connecting(connection_t *conn)
{
  unsigned char connected_payload[4];

  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_EXIT);
  tor_assert(conn->state == EXIT_CONN_STATE_CONNECTING);

  log_fn(LOG_INFO,"Exit connection to %s:%u established.",
         conn->address,conn->port);

  conn->state = EXIT_CONN_STATE_OPEN;
  connection_watch_events(conn, POLLIN); /* stop writing, continue reading */
  if (connection_wants_to_flush(conn)) /* in case there are any queued relay cells */
    connection_start_writing(conn);
  /* deliver a 'connected' relay cell back through the circuit. */
  if (connection_edge_is_rendezvous_stream(conn)) {
    if (connection_edge_send_command(conn, circuit_get_by_conn(conn),
                                     RELAY_COMMAND_CONNECTED, NULL, 0, conn->cpath_layer) < 0)
      return 0; /* circuit is closed, don't continue */
  } else {
    *(uint32_t*)connected_payload = htonl(conn->addr);
    if (connection_edge_send_command(conn, circuit_get_by_conn(conn),
        RELAY_COMMAND_CONNECTED, connected_payload, 4, conn->cpath_layer) < 0)
      return 0; /* circuit is closed, don't continue */
  }
  tor_assert(conn->package_window > 0);
  /* in case the server has written anything */
  return connection_edge_process_inbuf(conn, 1);
}

/** Find all general-purpose AP streams waiting for a response that sent
 * their begin/resolve cell >=15 seconds ago. Detach from their current circuit,
 * and mark their current circuit as unsuitable for new streams. Then call
 * connection_ap_handshake_attach_circuit() to attach to a new circuit (if
 * available) or launch a new one.
 *
 * For rendezvous streams, simply give up after 45 seconds (with no
 * retry attempt).
 */
void connection_ap_expire_beginning(void) {
  connection_t **carray;
  connection_t *conn;
  circuit_t *circ;
  int n, i;
  time_t now = time(NULL);
  or_options_t *options = get_options();

  get_connection_array(&carray, &n);

  for (i = 0; i < n; ++i) {
    conn = carray[i];
    if (conn->type != CONN_TYPE_AP)
      continue;
    if (conn->state != AP_CONN_STATE_RESOLVE_WAIT &&
        conn->state != AP_CONN_STATE_CONNECT_WAIT)
      continue;
    if (now - conn->timestamp_lastread < 15)
      continue;
    circ = circuit_get_by_conn(conn);
    if (!circ) { /* it's vanished? */
      log_fn(LOG_INFO,"Conn is waiting (address %s), but lost its circ.",
             conn->socks_request->address);
      connection_mark_for_close(conn);
      continue;
    }
    if (circ->purpose == CIRCUIT_PURPOSE_C_REND_JOINED) {
      if (now - conn->timestamp_lastread > 45) {
        log_fn(LOG_NOTICE,"Rend stream is %d seconds late. Giving up on address '%s'.",
               (int)(now - conn->timestamp_lastread), conn->socks_request->address);
        connection_edge_end(conn, END_STREAM_REASON_TIMEOUT, conn->cpath_layer);
        connection_mark_for_close(conn);
      }
      continue;
    }
    tor_assert(circ->purpose == CIRCUIT_PURPOSE_C_GENERAL);
    log_fn(LOG_NOTICE,"Stream is %d seconds late on address '%s'. Retrying.",
           (int)(now - conn->timestamp_lastread), conn->socks_request->address);
    circuit_log_path(LOG_NOTICE, circ);
    /* send an end down the circuit */
    connection_edge_end(conn, END_STREAM_REASON_TIMEOUT, conn->cpath_layer);
    /* un-mark it as ending, since we're going to reuse it */
    conn->has_sent_end = 0;
    /* move it back into 'pending' state. */
    conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
    circuit_detach_stream(circ, conn);
    /* kludge to make us not try this circuit again, yet to allow
     * current streams on it to survive if they can: make it
     * unattractive to use for new streams */
    tor_assert(circ->timestamp_dirty);
    circ->timestamp_dirty -= options->NewCircuitPeriod;
    /* give our stream another 15 seconds to try */
    conn->timestamp_lastread += 15;
    /* attaching to a dirty circuit is fine */
    if (connection_ap_handshake_attach_circuit(conn)<0) {
      /* it will never work */
      /* Don't need to send end -- we're not connected */
      conn->has_sent_end = 1;
      connection_mark_for_close(conn);
    }
  } /* end for */
}

/** Tell any AP streams that are waiting for a new circuit that one is
 * available.
 */
void connection_ap_attach_pending(void)
{
  connection_t **carray;
  connection_t *conn;
  int n, i;

  get_connection_array(&carray, &n);

  for (i = 0; i < n; ++i) {
    conn = carray[i];
    if (conn->marked_for_close ||
        conn->type != CONN_TYPE_AP ||
        conn->state != AP_CONN_STATE_CIRCUIT_WAIT)
      continue;
    if (connection_ap_handshake_attach_circuit(conn) < 0) {
      /* -1 means it will never work */
      /* Don't send end; there is no 'other side' yet */
      conn->has_sent_end = 1;
      connection_mark_for_close(conn);
    }
  }
}

/** connection_edge_process_inbuf() found a conn in state
 * socks_wait. See if conn->inbuf has the right bytes to proceed with
 * the socks handshake.
 *
 * If the handshake is complete, and it's for a general circuit, then
 * try to attach it to a circuit (or launch one as needed). If it's for
 * a rendezvous circuit, then fetch a rendezvous descriptor first (or
 * attach/launch a circuit if the rendezvous descriptor is already here
 * and fresh enough).
 *
 * Return -1 if an unexpected error with conn (and it should be marked
 * for close), else return 0.
 */
static int connection_ap_handshake_process_socks(connection_t *conn) {
  socks_request_t *socks;
  int sockshere;
  hostname_type_t addresstype;

  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_AP);
  tor_assert(conn->state == AP_CONN_STATE_SOCKS_WAIT);
  tor_assert(conn->socks_request);
  socks = conn->socks_request;

  log_fn(LOG_DEBUG,"entered.");

  sockshere = fetch_from_buf_socks(conn->inbuf, socks);
  if (sockshere == -1 || sockshere == 0) {
    if (socks->replylen) { /* we should send reply back */
      log_fn(LOG_DEBUG,"reply is already set for us. Using it.");
      connection_ap_handshake_socks_reply(conn, socks->reply, socks->replylen, 0);
      socks->replylen = 0; /* zero it out so we can do another round of negotiation */
    } else if (sockshere == -1) { /* send normal reject */
      log_fn(LOG_WARN,"Fetching socks handshake failed. Closing.");
      connection_ap_handshake_socks_reply(conn, NULL, 0, -1);
    } else {
      log_fn(LOG_DEBUG,"socks handshake not all here yet.");
    }
    if (sockshere == -1)
      socks->has_finished = 1;
    return sockshere;
  } /* else socks handshake is done, continue processing */

  /* Parse the address provided by SOCKS.  Modify it in-place if it
   * specifies a hidden-service (.onion) or particular exit node (.exit).
   */
  addresstype = parse_extended_hostname(socks->address);

  if (addresstype == EXIT_HOSTNAME) {
    /* .exit -- modify conn to specify the exit node. */
    char *s = strrchr(socks->address,'.');
    if (!s || s[1] == '\0') {
      log_fn(LOG_WARN,"Malformed address '%s.exit'. Refusing.", socks->address);
      return -1;
    }
    conn->chosen_exit_name = tor_strdup(s+1);
    *s = 0;
  }

  if (addresstype != ONION_HOSTNAME) {
    /* not a hidden-service request (i.e. normal or .exit) */

    if (socks->command == SOCKS_COMMAND_RESOLVE) {
      uint32_t answer = 0;
      struct in_addr in;
      /* Reply to resolves immediately if we can. */
      if (strlen(socks->address) > RELAY_PAYLOAD_SIZE) {
        log_fn(LOG_WARN,"Address to be resolved is too large. Failing.");
        connection_ap_handshake_socks_resolved(conn,RESOLVED_TYPE_ERROR,0,NULL);
        return -1;
      }
      if (tor_inet_aton(socks->address, &in)) /* see if it's an IP already */
        answer = in.s_addr;
      if (!answer && !conn->chosen_exit_name) /* if it's not .exit, check cache */
        answer = htonl(client_dns_lookup_entry(socks->address));
      if (answer) {
        connection_ap_handshake_socks_resolved(conn,RESOLVED_TYPE_IPV4,4,
                                               (char*)&answer);
        conn->has_sent_end = 1;
        connection_mark_for_close(conn);
        conn->hold_open_until_flushed = 1;
        return 0;
      }
    }

    if (socks->command == SOCKS_COMMAND_CONNECT && socks->port == 0) {
      log_fn(LOG_NOTICE,"Application asked to connect to port 0. Refusing.");
      return -1;
    }
    conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
    rep_hist_note_used_port(socks->port, time(NULL)); /* help predict this next time */
    return connection_ap_handshake_attach_circuit(conn);
  } else {
    /* it's a hidden-service request */
    rend_cache_entry_t *entry;
    int r;

    if (socks->command == SOCKS_COMMAND_RESOLVE) {
      /* if it's a resolve request, fail it right now, rather than
       * building all the circuits and then realizing it won't work. */
      log_fn(LOG_WARN,"Resolve requests to hidden services not allowed. Failing.");
      connection_ap_handshake_socks_resolved(conn,RESOLVED_TYPE_ERROR,0,NULL);
      return -1;
    }

    strlcpy(conn->rend_query, socks->address, sizeof(conn->rend_query));
    log_fn(LOG_INFO,"Got a hidden service request for ID '%s'", conn->rend_query);
    /* see if we already have it cached */
    r = rend_cache_lookup_entry(conn->rend_query, &entry);
    if (r<0) {
      log_fn(LOG_WARN,"Invalid service descriptor %s", conn->rend_query);
      return -1;
    }
    if (r==0) {
      conn->state = AP_CONN_STATE_RENDDESC_WAIT;
      log_fn(LOG_INFO, "Unknown descriptor %s. Fetching.", conn->rend_query);
      rend_client_refetch_renddesc(conn->rend_query);
      return 0;
    }
    if (r>0) {
#define NUM_SECONDS_BEFORE_REFETCH (60*15)
      if (time(NULL) - entry->received < NUM_SECONDS_BEFORE_REFETCH) {
        conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
        log_fn(LOG_INFO, "Descriptor is here and fresh enough. Great.");
        return connection_ap_handshake_attach_circuit(conn);
      } else {
        conn->state = AP_CONN_STATE_RENDDESC_WAIT;
        log_fn(LOG_INFO, "Stale descriptor %s. Refetching.", conn->rend_query);
        rend_client_refetch_renddesc(conn->rend_query);
        return 0;
      }
    }
  }
  return 0; /* unreached but keeps the compiler happy */
}

/** Iterate over the two bytes of stream_id until we get one that is not
 * already in use; return it. Return 0 if can't get a unique stream_id.
 */
static uint16_t get_unique_stream_id_by_circ(circuit_t *circ) {
  connection_t *tmpconn;
  uint16_t test_stream_id;
  uint32_t attempts=0;

again:
  test_stream_id = circ->next_stream_id++;
  if (++attempts > 1<<16) {
    /* Make sure we don't loop forever if all stream_id's are used. */
    log_fn(LOG_WARN,"No unused stream IDs. Failing.");
    return 0;
  }
  if (test_stream_id == 0)
    goto again;
  for (tmpconn = circ->p_streams; tmpconn; tmpconn=tmpconn->next_stream)
    if (tmpconn->stream_id == test_stream_id)
      goto again;
  return test_stream_id;
}

/** Write a relay begin cell, using destaddr and destport from ap_conn's
 * socks_request field, and send it down circ.
 *
 * If ap_conn is broken, mark it for close and return -1. Else return 0.
 */
int connection_ap_handshake_send_begin(connection_t *ap_conn, circuit_t *circ)
{
  char payload[CELL_PAYLOAD_SIZE];
  int payload_len;
  struct in_addr in;
  const char *string_addr;

  tor_assert(ap_conn->type == CONN_TYPE_AP);
  tor_assert(ap_conn->state == AP_CONN_STATE_CIRCUIT_WAIT);
  tor_assert(ap_conn->socks_request);

  ap_conn->stream_id = get_unique_stream_id_by_circ(circ);
  if (ap_conn->stream_id==0) {
    /* Don't send end: there is no 'other side' yet */
    ap_conn->has_sent_end = 1;
    connection_mark_for_close(ap_conn);
    circuit_mark_for_close(circ);
    return -1;
  }

  if (circ->purpose == CIRCUIT_PURPOSE_C_GENERAL) {
    in.s_addr = htonl(client_dns_lookup_entry(ap_conn->socks_request->address));
    string_addr = in.s_addr ? inet_ntoa(in) : NULL;

    tor_snprintf(payload,RELAY_PAYLOAD_SIZE,
             "%s:%d",
             string_addr ? string_addr : ap_conn->socks_request->address,
             ap_conn->socks_request->port);
  } else {
    tor_snprintf(payload,RELAY_PAYLOAD_SIZE,
             ":%d", ap_conn->socks_request->port);
  }
  payload_len = strlen(payload)+1;

  log_fn(LOG_DEBUG,"Sending relay cell to begin stream %d.",ap_conn->stream_id);

  if (connection_edge_send_command(ap_conn, circ, RELAY_COMMAND_BEGIN,
                                   payload, payload_len, ap_conn->cpath_layer) < 0)
    return -1; /* circuit is closed, don't continue */

  ap_conn->package_window = STREAMWINDOW_START;
  ap_conn->deliver_window = STREAMWINDOW_START;
  ap_conn->state = AP_CONN_STATE_CONNECT_WAIT;
  log_fn(LOG_INFO,"Address/port sent, ap socket %d, n_circ_id %d",ap_conn->s,circ->n_circ_id);
  control_event_stream_status(ap_conn, STREAM_EVENT_SENT_CONNECT);
  return 0;
}

/** Write a relay resolve cell, using destaddr and destport from ap_conn's
 * socks_request field, and send it down circ.
 *
 * If ap_conn is broken, mark it for close and return -1. Else return 0.
 */
int connection_ap_handshake_send_resolve(connection_t *ap_conn, circuit_t *circ)
{
  int payload_len;
  const char *string_addr;

  tor_assert(ap_conn->type == CONN_TYPE_AP);
  tor_assert(ap_conn->state == AP_CONN_STATE_CIRCUIT_WAIT);
  tor_assert(ap_conn->socks_request);
  tor_assert(ap_conn->socks_request->command == SOCKS_COMMAND_RESOLVE);
  tor_assert(circ->purpose == CIRCUIT_PURPOSE_C_GENERAL);

  ap_conn->stream_id = get_unique_stream_id_by_circ(circ);
  if (ap_conn->stream_id==0) {
    /* Don't send end: there is no 'other side' yet */
    ap_conn->has_sent_end = 1;
    connection_mark_for_close(ap_conn);
    circuit_mark_for_close(circ);
    return -1;
  }

  string_addr = ap_conn->socks_request->address;
  payload_len = strlen(string_addr);
  tor_assert(strlen(string_addr) <= RELAY_PAYLOAD_SIZE);

  log_fn(LOG_DEBUG,"Sending relay cell to begin stream %d.",ap_conn->stream_id);

  if (connection_edge_send_command(ap_conn, circ, RELAY_COMMAND_RESOLVE,
                           string_addr, payload_len, ap_conn->cpath_layer) < 0)
    return -1; /* circuit is closed, don't continue */

  ap_conn->state = AP_CONN_STATE_RESOLVE_WAIT;
  log_fn(LOG_INFO,"Address sent for resolve, ap socket %d, n_circ_id %d",ap_conn->s,circ->n_circ_id);
  control_event_stream_status(ap_conn, STREAM_EVENT_SENT_RESOLVE);
  return 0;
}

/** Make an AP connection_t, do a socketpair and attach one side
 * to the conn, connection_add it, initialize it to circuit_wait,
 * and call connection_ap_handshake_attach_circuit(conn) on it.
 *
 * Return the other end of the socketpair, or -1 if error.
 */
int connection_ap_make_bridge(char *address, uint16_t port) {
  int fd[2];
  connection_t *conn;

  log_fn(LOG_INFO,"Making AP bridge to %s:%d ...",address,port);

  if (tor_socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
    log(LOG_WARN,"Couldn't construct socketpair (%s). Network down? Delaying.",
        tor_socket_strerror(tor_socket_errno(-1)));
    return -1;
  }

  set_socket_nonblocking(fd[0]);
  set_socket_nonblocking(fd[1]);

  conn = connection_new(CONN_TYPE_AP);
  conn->s = fd[0];

  /* populate conn->socks_request */

  /* leave version at zero, so the socks_reply is empty */
  conn->socks_request->socks_version = 0;
  conn->socks_request->has_finished = 0; /* waiting for 'connected' */
  strlcpy(conn->socks_request->address, address,
          sizeof(conn->socks_request->address));
  conn->socks_request->port = port;
  conn->socks_request->command = SOCKS_COMMAND_CONNECT;

  conn->address = tor_strdup("(local bridge)");
  conn->addr = 0;
  conn->port = 0;

  if (connection_add(conn) < 0) { /* no space, forget it */
    connection_free(conn); /* this closes fd[0] */
    tor_close_socket(fd[1]);
    return -1;
  }

  conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
  connection_start_reading(conn);

  /* attaching to a dirty circuit is fine */
  if (connection_ap_handshake_attach_circuit(conn) < 0) {
    conn->has_sent_end = 1; /* no circ to send to */
    connection_mark_for_close(conn);
    tor_close_socket(fd[1]);
    return -1;
  }

  log_fn(LOG_INFO,"... AP bridge created and connected.");
  return fd[1];
}

/* DOCDOC */
void connection_ap_handshake_socks_resolved(connection_t *conn,
                                            int answer_type,
                                            size_t answer_len,
                                            const char *answer)
{
  char buf[256];
  size_t replylen;

  if (answer_type == RESOLVED_TYPE_IPV4) {
    uint32_t a = get_uint32(answer);
    if (a)
      client_dns_set_entry(conn->socks_request->address, ntohl(a));
  }

  if (conn->socks_request->socks_version == 4) {
    buf[0] = 0x00; /* version */
    if (answer_type == RESOLVED_TYPE_IPV4 && answer_len == 4) {
      buf[1] = 90; /* "Granted" */
      set_uint16(buf+2, 0);
      memcpy(buf+4, answer, 4); /* address */
      replylen = SOCKS4_NETWORK_LEN;
    } else {
      buf[1] = 91; /* "error" */
      memset(buf+2, 0, 6);
      replylen = SOCKS4_NETWORK_LEN;
    }
  } else {
    /* SOCKS5 */
    buf[0] = 0x05; /* version */
    if (answer_type == RESOLVED_TYPE_IPV4 && answer_len == 4) {
      buf[1] = 0; /* succeeded */
      buf[2] = 0; /* reserved */
      buf[3] = 0x01; /* IPv4 address type */
      memcpy(buf+4, answer, 4); /* address */
      set_uint16(buf+8, 0); /* port == 0. */
      replylen = 10;
    } else if (answer_type == RESOLVED_TYPE_IPV6 && answer_len == 16) {
      buf[1] = 0; /* succeeded */
      buf[2] = 0; /* reserved */
      buf[3] = 0x04; /* IPv6 address type */
      memcpy(buf+4, answer, 16); /* address */
      set_uint16(buf+20, 0); /* port == 0. */
      replylen = 22;
    } else {
      buf[1] = 0x04; /* host unreachable */
      memset(buf+2, 0, 8);
      replylen = 10;
    }
  }
  connection_ap_handshake_socks_reply(conn, buf, replylen,
                                      (answer_type == RESOLVED_TYPE_IPV4 ||
                                      answer_type == RESOLVED_TYPE_IPV6) ? 1 : -1);
  conn->socks_request->has_finished = 1;
}

/** Send a socks reply to stream <b>conn</b>, using the appropriate
 * socks version, etc.
 *
 * Status can be 1 (succeeded), -1 (failed), or 0 (not sure yet).
 *
 * If <b>reply</b> is defined, then write <b>replylen</b> bytes of it
 * to conn and return, else reply based on <b>status</b>.
 *
 * If <b>reply</b> is undefined, <b>status</b> can't be 0.
 */
void connection_ap_handshake_socks_reply(connection_t *conn, char *reply,
                                         size_t replylen, int status) {
  char buf[256];

  if (status) /* it's either 1 or -1 */
    control_event_stream_status(conn,
                       status==1 ? STREAM_EVENT_SUCCEEDED : STREAM_EVENT_FAILED);

  if (replylen) { /* we already have a reply in mind */
    connection_write_to_buf(reply, replylen, conn);
    return;
  }
  tor_assert(conn->socks_request);
  tor_assert(status == 1 || status == -1);
  if (conn->socks_request->socks_version == 4) {
    memset(buf,0,SOCKS4_NETWORK_LEN);
#define SOCKS4_GRANTED          90
#define SOCKS4_REJECT           91
    buf[1] = (status==1 ? SOCKS4_GRANTED : SOCKS4_REJECT);
    /* leave version, destport, destip zero */
    connection_write_to_buf(buf, SOCKS4_NETWORK_LEN, conn);
  }
  if (conn->socks_request->socks_version == 5) {
    buf[0] = 5; /* version 5 */
#define SOCKS5_SUCCESS          0
#define SOCKS5_GENERIC_ERROR    1
    buf[1] = status==1 ? SOCKS5_SUCCESS : SOCKS5_GENERIC_ERROR;
    buf[2] = 0;
    buf[3] = 1; /* ipv4 addr */
    memset(buf+4,0,6); /* Set external addr/port to 0.
                          The spec doesn't seem to say what to do here. -RD */
    connection_write_to_buf(buf,10,conn);
  }
  /* If socks_version isn't 4 or 5, don't send anything.
   * This can happen in the case of AP bridges. */
  return;
}

/** A relay 'begin' cell has arrived, and either we are an exit hop
 * for the circuit, or we are the origin and it is a rendezvous begin.
 *
 * Launch a new exit connection and initialize things appropriately.
 *
 * If it's a rendezvous stream, call connection_exit_connect() on
 * it.
 *
 * For general streams, call dns_resolve() on it first, and only call
 * connection_exit_connect() if the dns answer is already known.
 *
 * Note that we don't call connection_add() on the new stream! We wait
 * for connection_exit_connect() to do that.
 *
 * Return -1 if we want to tear down <b>circ</b>. Else return 0.
 */
int connection_exit_begin_conn(cell_t *cell, circuit_t *circ) {
  connection_t *n_stream;
  relay_header_t rh;
  char *address=NULL;
  uint16_t port;

  assert_circuit_ok(circ);
  relay_header_unpack(&rh, cell->payload);

  /* XXX currently we don't send an end cell back if we drop the
   * begin because it's malformed.
   */

  if (!memchr(cell->payload+RELAY_HEADER_SIZE, 0, rh.length)) {
    log_fn(LOG_WARN,"relay begin cell has no \\0. Dropping.");
    return 0;
  }
  if (parse_addr_port(cell->payload+RELAY_HEADER_SIZE,&address,NULL,&port)<0) {
    log_fn(LOG_WARN,"Unable to parse addr:port in relay begin cell. Dropping.");
    return 0;
  }
  if (port==0) {
    log_fn(LOG_WARN,"Missing port in relay begin cell. Dropping.");
    tor_free(address);
    return 0;
  }

  log_fn(LOG_DEBUG,"Creating new exit connection.");
  n_stream = connection_new(CONN_TYPE_EXIT);
  n_stream->purpose = EXIT_PURPOSE_CONNECT;

  n_stream->stream_id = rh.stream_id;
  n_stream->port = port;
  /* leave n_stream->s at -1, because it's not yet valid */
  n_stream->package_window = STREAMWINDOW_START;
  n_stream->deliver_window = STREAMWINDOW_START;

  if (circ->purpose == CIRCUIT_PURPOSE_S_REND_JOINED) {
    log_fn(LOG_DEBUG,"begin is for rendezvous. configuring stream.");
    n_stream->address = tor_strdup("(rendezvous)");
    n_stream->state = EXIT_CONN_STATE_CONNECTING;
    strlcpy(n_stream->rend_query, circ->rend_query,
            sizeof(n_stream->rend_query));
    tor_assert(connection_edge_is_rendezvous_stream(n_stream));
    assert_circuit_ok(circ);
    if (rend_service_set_connection_addr_port(n_stream, circ) < 0) {
      log_fn(LOG_INFO,"Didn't find rendezvous service (port %d)",n_stream->port);
      connection_edge_end(n_stream, END_STREAM_REASON_EXITPOLICY, n_stream->cpath_layer);
      connection_free(n_stream);
      circuit_mark_for_close(circ); /* knock the whole thing down, somebody screwed up */
      tor_free(address);
      return 0;
    }
    assert_circuit_ok(circ);
    log_fn(LOG_DEBUG,"Finished assigning addr/port");
    n_stream->cpath_layer = circ->cpath->prev; /* link it */

    /* add it into the linked list of n_streams on this circuit */
    n_stream->next_stream = circ->n_streams;
    circ->n_streams = n_stream;
    assert_circuit_ok(circ);

    connection_exit_connect(n_stream);
    tor_free(address);
    return 0;
  }
  n_stream->address = address;
  n_stream->state = EXIT_CONN_STATE_RESOLVEFAILED;
  /* default to failed, change in dns_resolve if it turns out not to fail */

  if (we_are_hibernating()) {
    connection_edge_end(n_stream, END_STREAM_REASON_EXITPOLICY, n_stream->cpath_layer);
    connection_free(n_stream);
    return 0;
  }

  /* send it off to the gethostbyname farm */
  switch (dns_resolve(n_stream)) {
    case 1: /* resolve worked */

      /* add it into the linked list of n_streams on this circuit */
      n_stream->next_stream = circ->n_streams;
      circ->n_streams = n_stream;
      assert_circuit_ok(circ);

      connection_exit_connect(n_stream);
      return 0;
    case -1: /* resolve failed */
      /* n_stream got freed. don't touch it. */
      break;
    case 0: /* resolve added to pending list */
      /* add it into the linked list of resolving_streams on this circuit */
      n_stream->next_stream = circ->resolving_streams;
      circ->resolving_streams = n_stream;
      assert_circuit_ok(circ);
      ;
  }
  return 0;
}

/**
 * Called when we receive a RELAY_RESOLVE cell 'cell' along the circuit 'circ';
 * begin resolving the hostname, and (eventually) reply with a RESOLVED cell.
 */
int connection_exit_begin_resolve(cell_t *cell, circuit_t *circ) {
  connection_t *dummy_conn;
  relay_header_t rh;

  assert_circuit_ok(circ);
  relay_header_unpack(&rh, cell->payload);

  /* This 'dummy_conn' only exists to remember the stream ID
   * associated with the resolve request; and to make the
   * implementation of dns.c more uniform.  (We really only need to
   * remember the circuit, the stream ID, and the hostname to be
   * resolved; but if we didn't store them in a connection like this,
   * the housekeeping in dns.c would get way more complicated.)
   */
  dummy_conn = connection_new(CONN_TYPE_EXIT);
  dummy_conn->stream_id = rh.stream_id;
  dummy_conn->address = tor_strndup(cell->payload+RELAY_HEADER_SIZE,
                                    rh.length);
  dummy_conn->port = 0;
  dummy_conn->state = EXIT_CONN_STATE_RESOLVEFAILED;
  dummy_conn->purpose = EXIT_PURPOSE_RESOLVE;

  /* send it off to the gethostbyname farm */
  switch (dns_resolve(dummy_conn)) {
    case -1: /* Impossible to resolve; a resolved cell was sent. */
      /* The connection got freed; leave it alone. */
      return 0;
    case 1: /* The result was cached; a resolved cell was sent. */
      connection_free(dummy_conn);
      return 0;
    case 0: /* resolve added to pending list */
      dummy_conn->next_stream = circ->resolving_streams;
      circ->resolving_streams = dummy_conn;
      assert_circuit_ok(circ);
      ;
  }
  return 0;
}

/** Connect to conn's specified addr and port. If it worked, conn
 * has now been added to the connection_array.
 *
 * Send back a connected cell. Include the resolved IP of the destination
 * address, but <em>only</em> if it's a general exit stream. (Rendezvous
 * streams must not reveal what IP they connected to.)
 */
void
connection_exit_connect(connection_t *conn) {
  unsigned char connected_payload[4];
  uint32_t addr;
  uint16_t port;

  if (!connection_edge_is_rendezvous_stream(conn) &&
      router_compare_to_my_exit_policy(conn) == ADDR_POLICY_REJECTED) {
    log_fn(LOG_INFO,"%s:%d failed exit policy. Closing.", conn->address, conn->port);
    connection_edge_end(conn, END_STREAM_REASON_EXITPOLICY, conn->cpath_layer);
    circuit_detach_stream(circuit_get_by_conn(conn), conn);
    connection_free(conn);
    return;
  }

  addr = conn->addr;
  port = conn->port;
  if (redirect_exit_list) {
    SMARTLIST_FOREACH(redirect_exit_list, exit_redirect_t *, r,
    {
      if ((addr&r->mask)==(r->addr&r->mask) &&
          (r->port_min <= port) && (port <= r->port_max)) {
        struct in_addr in;
        if (r->is_redirect) {
          addr = r->addr_dest;
          port = r->port_dest;
          in.s_addr = htonl(addr);
          log_fn(LOG_DEBUG, "Redirecting connection from %s:%d to %s:%d",
                 conn->address, conn->port, inet_ntoa(in), port);
        }
        break;
      }
    });
  }

  log_fn(LOG_DEBUG,"about to try connecting");
  switch (connection_connect(conn, conn->address, addr, port)) {
    case -1:
      connection_edge_end(conn, END_STREAM_REASON_CONNECTREFUSED, conn->cpath_layer);
      circuit_detach_stream(circuit_get_by_conn(conn), conn);
      connection_free(conn);
      return;
    case 0:
      conn->state = EXIT_CONN_STATE_CONNECTING;

      connection_watch_events(conn, POLLOUT | POLLIN | POLLERR);
      /* writable indicates finish, readable indicates broken link,
         error indicates broken link in windowsland. */
      return;
    /* case 1: fall through */
  }

  conn->state = EXIT_CONN_STATE_OPEN;
  if (connection_wants_to_flush(conn)) { /* in case there are any queued data cells */
    log_fn(LOG_WARN,"Bug: newly connected conn had data waiting!");
//    connection_start_writing(conn);
  }
  connection_watch_events(conn, POLLIN);

  /* also, deliver a 'connected' cell back through the circuit. */
  if (connection_edge_is_rendezvous_stream(conn)) { /* rendezvous stream */
    /* don't send an address back! */
    connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED,
                                 NULL, 0, conn->cpath_layer);
  } else { /* normal stream */
    /* This must be the original address, not the redirected address. */
    *(uint32_t*)connected_payload = htonl(conn->addr);
    connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED,
                                 connected_payload, 4, conn->cpath_layer);
  }
}

/** Return 1 if <b>conn</b> is a rendezvous stream, or 0 if
 * it is a general stream.
 */
int connection_edge_is_rendezvous_stream(connection_t *conn) {
  tor_assert(conn);
  if (*conn->rend_query) /* XXX */
    return 1;
  return 0;
}

/** Return 1 if router <b>exit</b> might allow stream <b>conn</b>
 * to exit from it, or 0 if it definitely will not allow it.
 * (We might be uncertain if conn's destination address has not yet been
 * resolved.)
 */
int connection_ap_can_use_exit(connection_t *conn, routerinfo_t *exit)
{
  uint32_t addr;

  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_AP);
  tor_assert(conn->socks_request);
  tor_assert(exit);

  log_fn(LOG_DEBUG,"considering nickname %s, for address %s / port %d:",
         exit->nickname, conn->socks_request->address,
         conn->socks_request->port);

  /* If a particular exit node has been requested for the new connection,
   * make sure the exit node of the existing circuit matches exactly.
   */
  if (conn->chosen_exit_name) {
    if (router_get_by_nickname(conn->chosen_exit_name) != exit) {
      /* doesn't match */
      log_fn(LOG_DEBUG,"Requested node '%s', considering node '%s'. No.",
             conn->chosen_exit_name, exit->nickname);
      return 0;
    }
  }

  if (conn->socks_request->command == SOCKS_COMMAND_RESOLVE) {
    /* 0.0.8 servers have buggy resolve support. */
    if (!tor_version_as_new_as(exit->platform, "0.0.9pre1"))
      return 0;
  } else {
    addr = client_dns_lookup_entry(conn->socks_request->address);
    if (router_compare_addr_to_addr_policy(addr, conn->socks_request->port,
          exit->exit_policy) == ADDR_POLICY_REJECTED)
      return 0;
  }
  return 1;
}

/** A helper function for socks_policy_permits_address() below.
 *
 * Parse options->SocksPolicy in the same way that the exit policy
 * is parsed, and put the processed version in &socks_policy.
 * Ignore port specifiers.
 */
void
parse_socks_policy(void)
{
  addr_policy_t *n;
  if (socks_policy) {
    addr_policy_free(socks_policy);
    socks_policy = NULL;
  }
  config_parse_addr_policy(get_options()->SocksPolicy, &socks_policy);
  /* ports aren't used. */
  for (n=socks_policy; n; n = n->next) {
    n->prt_min = 1;
    n->prt_max = 65535;
  }
}

/** Return 1 if <b>addr</b> is permitted to connect to our socks port,
 * based on <b>socks_policy</b>. Else return 0.
 */
int socks_policy_permits_address(uint32_t addr)
{
  int a;

  if (!socks_policy) /* 'no socks policy' means 'accept' */
    return 1;
  a = router_compare_addr_to_addr_policy(addr, 1, socks_policy);
  if (a==-1)
    return 0;
  else if (a==0)
    return 1;
  tor_assert(a==1);
  log_fn(LOG_WARN, "Bug: Got unexpected 'maybe' answer from socks policy");
  return 0;
}

/* ***** Client DNS code ***** */

/* XXX Perhaps this should get merged with the dns.c code somehow. */
/* XXX But we can't just merge them, because then nodes that act as
 *     both OR and OP could be attacked: people could rig the dns cache
 *     by answering funny things to stream begin requests, and later
 *     other clients would reuse those funny addr's. Hm.
 */

/** A client-side struct to remember the resolved IP (addr) for
 * a given address. These structs make up a tree, with client_dns_map
 * below as its root.
 */
struct client_dns_entry {
  uint32_t addr; /**< The resolved IP of this entry. */
  time_t expires; /**< At what second does addr expire? */
  int n_failures; /**< How many times has this entry failed to resolve so far? */
};

/** How many elements are in the client dns cache currently? */
static int client_dns_size = 0;
/** The tree of client-side cached DNS resolves. */
static strmap_t *client_dns_map = NULL;

/** Initialize client_dns_map and client_dns_size. */
void client_dns_init(void) {
  client_dns_map = strmap_new();
  client_dns_size = 0;
}

/** Return the client_dns_entry that corresponds to <b>address</b>.
 * If it's not there, allocate and return a new entry for <b>address</b>.
 */
static struct client_dns_entry *
_get_or_create_ent(const char *address)
{
  struct client_dns_entry *ent;
  ent = strmap_get_lc(client_dns_map,address);
  if (!ent) {
    ent = tor_malloc_zero(sizeof(struct client_dns_entry));
    ent->expires = time(NULL)+MAX_DNS_ENTRY_AGE;
    strmap_set_lc(client_dns_map,address,ent);
    ++client_dns_size;
  }
  return ent;
}

/** Return the IP associated with <b>address</b>, if we know it
 * and it's still fresh enough. Otherwise return 0.
 */
uint32_t client_dns_lookup_entry(const char *address)
{
  struct client_dns_entry *ent;
  struct in_addr in;
  time_t now;

  tor_assert(address);

  if (tor_inet_aton(address, &in)) {
    log_fn(LOG_DEBUG, "Using static address %s (%08lX)", address,
           (unsigned long)ntohl(in.s_addr));
    return ntohl(in.s_addr);
  }
  ent = strmap_get_lc(client_dns_map,address);
  if (!ent || !ent->addr) {
    log_fn(LOG_DEBUG, "No entry found for address %s", address);
    return 0;
  } else {
    now = time(NULL);
    if (ent->expires < now) {
      log_fn(LOG_DEBUG, "Expired entry found for address %s", address);
      strmap_remove_lc(client_dns_map,address);
      tor_free(ent);
      --client_dns_size;
      return 0;
    }
    in.s_addr = htonl(ent->addr);
    log_fn(LOG_DEBUG, "Found cached entry for address %s: %s", address,
           inet_ntoa(in));
    return ent->addr;
  }
}

/** An attempt to resolve <b>address</b> failed at some OR.
 * Increment the number of resolve failures we have on record
 * for it, and then return that number.
 */
int client_dns_incr_failures(const char *address)
{
  struct client_dns_entry *ent;
  ent = _get_or_create_ent(address);
  ++ent->n_failures;
  log_fn(LOG_DEBUG,"Address %s now has %d resolve failures.",
         address, ent->n_failures);
  return ent->n_failures;
}

/** Record the fact that <b>address</b> resolved to <b>val</b>.
 * We can now use this in subsequent streams in client_dns_lookup_entry(),
 * so we can more correctly choose a router that will allow <b>address</b>
 * to exit from him.
 */
void client_dns_set_entry(const char *address, uint32_t val)
{
  struct client_dns_entry *ent;
  struct in_addr in;
  time_t now;

  tor_assert(address);
  tor_assert(val);

  if (tor_inet_aton(address, &in))
    return;
  now = time(NULL);
  ent = _get_or_create_ent(address);
  in.s_addr = htonl(val);
  log_fn(LOG_DEBUG, "Updating entry for address %s: %s", address,
         inet_ntoa(in));
  ent->addr = val;
  ent->expires = now+MAX_DNS_ENTRY_AGE;
  ent->n_failures = 0;
}

/** A helper function for client_dns_clean() below. If ent is too old,
 * then remove it from the tree and return NULL, else return ent.
 */
static void* _remove_if_expired(const char *addr,
                                struct client_dns_entry *ent,
                                time_t *nowp)
{
  if (ent->expires < *nowp) {
    --client_dns_size;
    tor_free(ent);
    return NULL;
  } else {
    return ent;
  }
}

/** Clean out entries from the client-side DNS cache that were
 * resolved long enough ago that they are no longer valid.
 */
void client_dns_clean(void)
{
  time_t now;

  if (!client_dns_size)
    return;
  now = time(NULL);
  strmap_foreach(client_dns_map, (strmap_foreach_fn)_remove_if_expired, &now);
}

/** Make connection redirection follow the provided list of
 * exit_redirect_t */
void
set_exit_redirects(smartlist_t *lst)
{
  if (redirect_exit_list) {
    SMARTLIST_FOREACH(redirect_exit_list, exit_redirect_t *, p, tor_free(p));
    smartlist_free(redirect_exit_list);
  }
  redirect_exit_list = lst;
}

/** If address is of the form "y.onion" with a well-formed handle y:
 *     Put a '\0' after y, lower-case it, and return ONION_HOSTNAME.
 *
 * If address is of the form "y.exit":
 *     Put a '\0' after y and return EXIT_HOSTNAME.
 *
 * Otherwise:
 *     Return NORMAL_HOSTNAME and change nothing.
 */
hostname_type_t
parse_extended_hostname(char *address) {
    char *s;
    char query[REND_SERVICE_ID_LEN+1];

    s = strrchr(address,'.');
    if (!s) return 0; /* no dot, thus normal */
    if (!strcasecmp(s+1,"exit")) {
      *s = 0; /* null-terminate it */
      return EXIT_HOSTNAME; /* .exit */
    }
    if (strcasecmp(s+1,"onion"))
      return NORMAL_HOSTNAME; /* neither .exit nor .onion, thus normal */

    /* so it is .onion */
    *s = 0; /* null-terminate it */
    if (strlcpy(query, address, REND_SERVICE_ID_LEN+1) >= REND_SERVICE_ID_LEN+1)
      goto failed;
    tor_strlower(query);
    if (rend_valid_service_id(query)) {
      tor_strlower(address);
      return ONION_HOSTNAME; /* success */
    }
failed:
    /* otherwise, return to previous state and return 0 */
    *s = '.';
    return NORMAL_HOSTNAME;
}

